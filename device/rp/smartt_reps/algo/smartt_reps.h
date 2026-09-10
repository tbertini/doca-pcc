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

#ifndef SMARTT_REPS_H
#define SMARTT_REPS_H

/*
 * Device-side interface of the SMaRTT congestion-control algorithm: entry points, trace
 * format IDs and the trace-argument encodings shared by the DPA entry-point file and the
 * algorithm core.
 *
 * Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed & Timed
 * Transport with Recycled Entropies", arXiv:2404.01630, 2024.
 */

#include "smartt_reps_params.h"	/* SMARTT_REPS_P_TRACE_ON */

/*
 * Runtime trace gate. The format 20-24 device traces are compiled in unconditionally and
 * gated here, so the trace_on PPCC param toggles them at runtime with no DPA rebuild;
 * default is off. Building with -DSMARTT_REPS_DEBUG forces them on from boot, ignoring
 * the param.
 */
#ifdef SMARTT_REPS_DEBUG
#define SMARTT_REPS_TRACE_ENABLED(param) (1)
#else
#define SMARTT_REPS_TRACE_ENABLED(param) ((param)[SMARTT_REPS_P_TRACE_ON] != 0)
#endif

/*
 * arg1 packing for every SMaRTT trace call (format 6 and 20-24): high 32 bits = event
 * timestamp in ns (wraps at 2^32 ns, about 4.295 s), low 32 bits = flow_tag. The host
 * prints it as tsflow=0x%lx. ts and flow must come from doca_pcc_dev_get_timestamp(event)
 * and doca_pcc_dev_get_flowtag(event).
 */
#define SMARTT_REPS_TRACE_TSFLOW(ts, flow) ((((uint64_t)(uint32_t)(ts)) << 32) | (uint64_t)(uint32_t)(flow))

/*
 * Trace format IDs owned by SMaRTT, emitted when SMARTT_REPS_TRACE_ENABLED(param) is
 * true; per-event summaries reuse the shared generic format 6.
 *
 * doca_pcc_dev_trace_5 carries five 64-bit args, and the argument orders below must match
 * the host pcc_core.c format strings exactly. arg1 is always SMARTT_REPS_TRACE_TSFLOW().
 */
#define SMARTT_REPS_TRACE_FMT_ACK     (20) /* per ACK decision: tsflow, branch, rtt_ns, cwnd_fxp16(after), rate_fxp20 */
#define SMARTT_REPS_TRACE_FMT_QA      (21) /* QuickAdapt fire: tsflow, src(1=ack,2=nack), ignore_target, new_cwnd, rate */
#define SMARTT_REPS_TRACE_FMT_FASTINC (22) /* FastIncrease active: tsflow, cnt_ncoal(fast_inc_count<<16|ncoal), rtt_ns, cwnd, rate */
#define SMARTT_REPS_TRACE_FMT_SIG     (23) /* raw ACK signal: tsflow, first_sn, delta_ncoal(delta_pkts<<16|ncoal), avg_wtd_fxp16, 0 */
#define SMARTT_REPS_TRACE_FMT_LOSS    (24) /* NACK/loss: tsflow, nack_first_sn, ign_qa(ignored_pkts<<1|trigger_qa), cwnd(after), rate */

/*
 * Branch identifiers used in the format-20 trace.
 * Low nibble = which control-loop path the ACK event took.
 * High bits annotate edge conditions (OR-ed in).
 */
#define SMARTT_REPS_BRANCH_FD          (0)    /* Fair Decrease (paper Section 3.2.1) */
#define SMARTT_REPS_BRANCH_MD          (1)    /* Multiplicative Decrease + FD (paper Section 3.2.2) */
#define SMARTT_REPS_BRANCH_FI          (2)    /* Fair Increase (paper Section 3.2.3) */
#define SMARTT_REPS_BRANCH_MI          (3)    /* Multiplicative Increase + FI (paper Section 3.2.4) */
#define SMARTT_REPS_BRANCH_WTD_BLOCKED (4)    /* ECN decrease wanted but avg_wtd < thresh */
#define SMARTT_REPS_BRANCH_BLACKOUT    (5)    /* post-QuickAdapt blackout: ACK swallowed */
#define SMARTT_REPS_BRANCH_FASTINC     (6)    /* FastIncrease fired this ACK */
#define SMARTT_REPS_BRANCH_QA_ADAPTED  (7)    /* QuickAdapt fired this ACK */
#define SMARTT_REPS_BRANCH_CLAMPED_BIT (0x80) /* actuated rate hit min_rate or MAX_RATE */
#define SMARTT_REPS_BRANCH_ECN_BIT     (0x40) /* p.ecn asserted (CNP-hold predicate true) */
#define SMARTT_REPS_BRANCH_CAN_DEC_BIT (0x20) /* can_decrease (avg_wtd >= wtd_thresh) */

/*
 * Entry point to SMaRTT user algorithm.
 * Runs the algorithm for a single event, computing the new cwnd (paper Algorithms 1, 2
 * and 3) and the FXP20 rate it actuates to.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @param [in]: A pointer to the array of PPCC parameters
 * @counter [in/out]: A pointer to the array of PPCC counters (incremented by algo)
 * @algo_ctxt [in/out]: A pointer to the per-flow context retrieved by libpcc.
 * @results [out]: A pointer to the result struct to update rate in HW.
 */
void smartt_reps_algo(doca_pcc_dev_event_t *event,
		      uint32_t *param,
		      uint32_t *counter,
		      doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		      doca_pcc_dev_results_t *results);

/*
 * Entry point to SMaRTT user algorithm initialization.
 * Called once per process load; registers the algorithm metadata, parameters, and
 * counters for the given algo slot index.
 *
 * @algo_idx [in]: Algo identifier, passed on to the init APIs.
 */
void smartt_reps_init(uint32_t algo_idx);

/*
 * Entry point to SMaRTT user algorithm parameter setting.
 * Validates externally-set parameter values before the framework assigns them.
 *
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of parameters that were changed
 * @new_param_values [in]: array of param_num new values
 * @params [in]: array holding the current parameters to be changed
 * @return: DOCA_PCC_DEV_STATUS_FAIL if one or more input values are not legal.
 */
doca_pcc_dev_error_t smartt_reps_set_algo_params(uint32_t param_id_base,
						 uint32_t param_num,
						 const uint32_t *new_param_values,
						 uint32_t *params);

#endif /* SMARTT_REPS_H */
