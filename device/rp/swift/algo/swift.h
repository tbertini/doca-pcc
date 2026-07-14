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

#ifndef SWIFT_H
#define SWIFT_H

#include "swift_params.h"	/* SWIFT_P_TRACE_ON */

/*
 * Runtime trace gate.
 *
 * Swift's format 15-19 device traces are compiled in unconditionally and gated
 * here, so they can be toggled at runtime via the trace_on PPCC param
 * (param[SWIFT_P_TRACE_ON]) WITHOUT rebuilding the DPA binary. Default is off.
 *
 * Building with -DSWIFT_DEBUG forces them on from boot (e.g. to capture the earliest
 * events, before a param write can land), ignoring the param.
 *
 * Cost when off: one well-predicted branch per event. The trace-record write and the
 * buffer flush -- the expensive part -- are skipped.
 */
#ifdef SWIFT_DEBUG
#define SWIFT_TRACE_ENABLED(param) (1)
#else
#define SWIFT_TRACE_ENABLED(param) ((param)[SWIFT_P_TRACE_ON] != 0)
#endif

/*
 * Packed tsflow trace argument (trace-format normalization, 2026-07-10).
 *
 * Every Swift trace line (formats 6, 15-19) carries this as its FIRST doca_pcc_dev_trace_5
 * arg -- printed host-side as tsflow=0x%lx -- instead of a separate timestamp and/or
 * flow_tag field at whatever position each format used to put them. Packing:
 *   bits 63:32 = ts_ns   (the uint32_t doca_pcc_dev_get_timestamp() value; wraps ~4.295s)
 *   bits 31:0  = flow_tag (the uint32_t doca_pcc_dev_get_flowtag() value)
 * Same event -> same tsflow: lines emitted for one RTT event (formats 19, 16, 17) carry
 * an identical tsflow, so a reader can join them by value instead of relying on log order.
 */
#define SWIFT_TRACE_TSFLOW(ts, flow) ((((uint64_t)(uint32_t)(ts)) << 32) | (uint64_t)(uint32_t)(flow))

/*
 * Trace format IDs (emitted when SWIFT_TRACE_ENABLED(param) is true).
 *
 * Per-event summaries reuse the shared generic format 6 (see rp_swift_dev_main.c), as
 * TIMELY does. Format IDs 6-14 are owned by the generic/DCQCN/TIMELY traces in the
 * merged trace_message_formats[] table; Swift claims 15-19.
 *
 * The two RTT-decision lines are split because doca_pcc_dev_trace_5 carries only five
 * 64-bit args and the per-event decision has more than five fields worth checking by
 * hand: format 16 carries the delay/target side, format 17 the cwnd/rate side. They
 * share the event so a reader joins them by timestamp/order (and, since the tsflow
 * normalization, by identical tsflow value too). Format 19 fires for every probe
 * response, including samples dropped for missing/inconsistent responder stamps; 16/17
 * fire only for usable samples, so a 19 line may stand alone.
 *
 * Field lists below are in wire/print order (arg1 tsflow always first; rate last where
 * a format has one). A few fields dropped as host-derivable to make room -- see the
 * per-call comments in swift.c and the trace-format table in README.md.
 */
#define SWIFT_TRACE_FMT_NEW_FLOW   (15)  /* tsflow, init_cwnd, line_mbps, bdp_fxp16, rate */
#define SWIFT_TRACE_FMT_RTT_DELAY  (16)  /* tsflow, fabric_rtt, np_turnaround, target, branch */
#define SWIFT_TRACE_FMT_RTT_WINDOW (17)  /* tsflow, cwnd, fs_term, can_decrease, rate */
#define SWIFT_TRACE_FMT_LOSS       (18)  /* tsflow, ev_subtype, retx_reset, cwnd, rate; always emitted (ungated) */
#define SWIFT_TRACE_FMT_RTT_TS     (19)  /* tsflow (ts half = t6/arrival), req_send, req_recv, resp_send, populated */

/*
 * Branch identifiers used in the format-16 trace.
 * Low 4 bits = which Algorithm-1 path the RTT event took.
 * High bits annotate edge conditions (OR-ed in).
 */
#define SWIFT_BRANCH_AI         (0)     /* delay <  target -> additive increase */
#define SWIFT_BRANCH_MD         (1)     /* delay >= target, can_decrease -> multiplicative decrease */
#define SWIFT_BRANCH_MD_BLOCKED (2)     /* delay >= target, but once-per-RTT guard blocked the MD */
#define SWIFT_BRANCH_CLAMPED_BIT (0x80) /* actuated rate hit min_rate or MAX_RATE */
#define SWIFT_BRANCH_CWND_LT1_BIT (0x40)/* cwnd ended < 1 packet (pacing regime) */
#define SWIFT_BRANCH_FS_SAT_BIT (0x20)  /* flow-scale term saturated (0 or fs_range) */

/*
 * Entry point to Swift user algorithm.
 * Runs the algorithm for a single event, computing the new cwnd (per Algorithm 1,
 * Kumar et al. SIGCOMM 2020) and the FXP20 rate it actuates to.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @param [in]: A pointer to the array of PPCC parameters (see swift_params.h)
 * @counter [in/out]: A pointer to the array of PPCC counters (incremented by algo)
 * @algo_ctxt [in/out]: A pointer to the per-flow context retrieved by libpcc.
 * @results [out]: A pointer to the result struct to update rate in HW.
 */
void swift_algo(doca_pcc_dev_event_t *event,
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
void swift_init(uint32_t algo_idx);

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
doca_pcc_dev_error_t swift_set_algo_params(uint32_t param_id_base,
					   uint32_t param_num,
					   const uint32_t *new_param_values,
					   uint32_t *params);

#endif /* SWIFT_H */
