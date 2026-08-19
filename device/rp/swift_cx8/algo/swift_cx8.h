/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef SWIFT_CX8_H
#define SWIFT_CX8_H

#include "swift_cx8_params.h"	/* SWIFT_CX8_P_TRACE_ON */

/*
 * Runtime trace gate: formats 6, 15-17 and 19 are compiled in and toggled at runtime
 * via the trace_on PPCC param (default off, no rebuild needed). Format 18 (loss/NACK)
 * is never gated. Building with -DSWIFT_CX8_DEBUG forces traces on from boot.
 */
#ifdef SWIFT_CX8_DEBUG
#define SWIFT_CX8_TRACE_ENABLED(param) (1)
#else
#define SWIFT_CX8_TRACE_ENABLED(param) ((param)[SWIFT_CX8_P_TRACE_ON] != 0)
#endif

/*
 * tsflow: the first trace arg of every Swift format (6, 15-19).
 *   bits 63:32 = doca_pcc_dev_get_timestamp() (ns, wraps ~4.295 s)
 *   bits 31:0  = flow_tag
 * Lines emitted for one event carry an identical tsflow, so a reader joins them by value.
 */
#define SWIFT_CX8_TRACE_TSFLOW(ts, flow) ((((uint64_t)(uint32_t)(ts)) << 32) | (uint64_t)(uint32_t)(flow))

/*
 * Trace format IDs. Format 6 is the shared per-event summary; IDs 6-14 belong to the
 * generic/DCQCN/TIMELY traces, Swift claims 15-19 (table in README.md).
 * The decision is split over formats 16 (delay/target side) and 17 (cwnd/rate side)
 * because a trace line carries only five args; join them by tsflow. Unlike
 * device/rp/swift/, 16/17 fire from the ACK handler (every decrease, 1-in-64 additive
 * increases), so format 19 (probe responses, including dropped samples) stands alone.
 * FIELD REMAP: format 16's third field carries num_acked here, not np_turnaround; the
 * shared host format string still labels it np_turn=.
 */
#define SWIFT_CX8_TRACE_FMT_NEW_FLOW   (15)  /* tsflow, init_cwnd, line_mbps, bdp_fxp16, rate */
#define SWIFT_CX8_TRACE_FMT_RTT_DELAY  (16)  /* tsflow, fabric_rtt, num_acked, target, branch */
#define SWIFT_CX8_TRACE_FMT_RTT_WINDOW (17)  /* tsflow, cwnd, fs_term, can_decrease, rate */
#define SWIFT_CX8_TRACE_FMT_LOSS       (18)  /* tsflow, ev_subtype, retx_reset, cwnd, rate; always emitted (ungated) */
#define SWIFT_CX8_TRACE_FMT_RTT_TS     (19)  /* tsflow (ts half = t6/arrival), req_send, req_recv, resp_send, populated */

/*
 * Branch identifiers used in the format-16 trace.
 * Low 4 bits = which Algorithm-1 path the ACK event took.
 * High bits annotate edge conditions (OR-ed in).
 */
#define SWIFT_CX8_BRANCH_AI         (0)     /* delay <  target -> additive increase */
#define SWIFT_CX8_BRANCH_MD         (1)     /* delay >= target, can_decrease -> multiplicative decrease */
#define SWIFT_CX8_BRANCH_MD_BLOCKED (2)     /* delay >= target, but once-per-RTT guard blocked the MD */
#define SWIFT_CX8_BRANCH_CLAMPED_BIT (0x80) /* cwnd hit min_cwnd or max_cwnd (the rate limiter no
					     * longer clamps: it is pinned open above 1 packet) */
#define SWIFT_CX8_BRANCH_CWND_LT1_BIT (0x40)/* cwnd ended < 1 packet (pacing regime) */
#define SWIFT_CX8_BRANCH_FS_SAT_BIT (0x20)  /* flow-scale term saturated (0 or fs_range) */

/*
 * Entry point to Swift user algorithm.
 * Runs the algorithm for a single event, computing the new cwnd (per Algorithm 1,
 * Kumar et al. SIGCOMM 2020) and the FXP20 rate it actuates to.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @param [in]: A pointer to the array of PPCC parameters (see swift_cx8_params.h)
 * @counter [in/out]: A pointer to the array of PPCC counters (incremented by algo)
 * @algo_ctxt [in/out]: A pointer to the per-flow context retrieved by libpcc.
 * @results [out]: A pointer to the result struct to update rate in HW.
 */
void swift_cx8_algo(doca_pcc_dev_event_t *event,
		uint32_t *param,
		uint32_t *counter,
		doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		doca_pcc_dev_results_t *results);

/*
 * Entry point to Swift user algorithm initialization.
 * Called once per process load; registers the algorithm metadata, parameters, and
 * counters for the given algo slot index.
 *
 * @algo_idx [in]: Algo identifier, passed on to the init APIs.
 */
void swift_cx8_init(uint32_t algo_idx);

/*
 * Entry point to Swift user algorithm parameter setting.
 * Validates externally-set parameter values before the framework assigns them.
 *
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of parameters that were changed
 * @new_param_values [in]: array of param_num new values
 * @params [in]: array holding the current parameters to be changed
 * @return: DOCA_PCC_DEV_STATUS_FAIL if one or more input values are not legal.
 */
doca_pcc_dev_error_t swift_cx8_set_algo_params(uint32_t param_id_base,
					   uint32_t param_num,
					   const uint32_t *new_param_values,
					   uint32_t *params);

#endif /* SWIFT_CX8_H */
