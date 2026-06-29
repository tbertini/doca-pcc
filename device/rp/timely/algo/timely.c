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

/*
 * TIMELY: RTT-based congestion control algorithm
 *
 * Implementation of the RP (Reaction Point) algorithm from:
 * Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter", SIGCOMM 2015.
 *
 * Reference: Algorithm 1 (§4.3) and parameter table (§6).
 *
 * TIMELY is a rate-based, RTT-driven congestion controller. It runs on each
 * completion event (RTT measurement) and adjusts the per-flow rate based on
 * the smoothed delay gradient (rate of change of RTT). Unlike DCQCN, it does
 * NOT use ECN/CNPs — RTT is the only congestion signal.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "timely_ctxt.h"
#include "timely_params.h"
#include "timely.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Per-event trace emission, gated by TIMELY_TRACE_ENABLED(param) (see timely.h):
 * compiled in unconditionally, emitted only when the trace_on PPCC param is set at
 * runtime (or always under -DTIMELY_DEBUG). The flush is gated too -- flushing with
 * nothing buffered is wasted work on the hot path.
 * WARNING: when enabled this adds significant overhead on every RTT event. */
#define TIMELY_TRACE(param, fmt_id, a1, a2, a3, a4, a5) \
	do { \
		if (TIMELY_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_5(fmt_id, (uint64_t)(a1), (uint64_t)(a2), \
					     (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5)); \
	} while (0)
#define TIMELY_TRACE_FLUSH(param) \
	do { \
		if (TIMELY_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_flush(); \
	} while (0)

/* Trace format IDs (emitted when TIMELY_TRACE_ENABLED(param) is true; see timely.h).
 * Format IDs 7-10 are reserved for DCQCN (so the two algorithms can share a
 * single trace_message_formats[] table when both branches are merged). */
#define TIMELY_TRACE_FMT_NEW_FLOW (11)
#define TIMELY_TRACE_FMT_RTT      (12)
/* Format ID 13 retired — clamp info now folded into format 12's branch_taken */
#define TIMELY_TRACE_FMT_HAI      (14)

/* Branch identifiers used in the format-12 trace.
 * Low 4 bits = which Algorithm-1 branch was taken.
 * Bit 7 (0x80) = rate was clamped (to either MIN_RATE or DOCA_PCC_DEV_MAX_RATE). */
#define TIMELY_BRANCH_T_LOW       (0)
#define TIMELY_BRANCH_T_HIGH      (1)
#define TIMELY_BRANCH_GRAD_LE     (2)
#define TIMELY_BRANCH_GRAD_GT     (3)
#define TIMELY_BRANCH_CLAMPED_BIT (0x80)

/* Abort time for stuck RTT requests (mirrors rtt_template) */
#define TIMELY_ABORT_TIME_NS (300000)

/* FXP16 1.0, and the HAI-mode increase multiplier (paper §4.3: rate += N·δ, N=5).
 * The only algorithm constants not part of the PPCC contract in timely_params.h. */
#define TIMELY_FXP16_ONE (1 << 16)
#define TIMELY_HAI_FACTOR (5)

/*
 * Calculate wrap around in case of int overflow.
 * Local static copy to avoid duplicate symbol with pcc_common_dev.h under LTO
 * (the pcc_common_dev.h version is ALWAYS_INLINE which doesn't survive -flto
 * when included in two translation units).
 */
static inline uint32_t timely_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* Description strings (kept volatile to prevent dead-code elimination by LTO) */
const volatile char timely_desc[] = "TIMELY v1.0 (Mittal et al. SIGCOMM 2015)";
static const volatile char timely_param_t_low_desc[] = "T_LOW, RTT low threshold in ns";
static const volatile char timely_param_t_high_desc[] = "T_HIGH, RTT high threshold in ns";
static const volatile char timely_param_delta_desc[] = "DELTA, additive increment step (FXP20 of line rate)";
static const volatile char timely_param_beta_desc[] = "BETA, multiplicative decrement factor (FXP16)";
static const volatile char timely_param_alpha_desc[] = "ALPHA, EWMA weight for rtt_diff (FXP16)";
static const volatile char timely_param_min_rtt_desc[] = "MIN_RTT_NS, wire propagation delay in ns";
static const volatile char timely_param_hai_threshold_desc[] = "HAI_THRESHOLD, consecutive neg-gradient events for HAI";
static const volatile char timely_param_min_rate_desc[] = "MIN_RATE, minimum rate floor (FXP20 of line rate)";
static const volatile char timely_param_init_rate_desc[] = "INIT_RATE, initial rate for new flows (FXP20 of line rate)";
static const volatile char timely_param_trace_on_desc[] = "TRACE_ON, runtime diagnostic trace toggle (0=off, 1=on); always on if built -DTIMELY_DEBUG";
static const volatile char timely_counter_events_desc[] = "EVENTS, total events dispatched";
static const volatile char timely_counter_md_gradient_desc[] = "MD_GRADIENT, multiplicative decrease on positive RTT gradient";
static const volatile char timely_counter_md_thigh_desc[] = "MD_THIGH, multiplicative decrease on RTT > T_high";
static const volatile char timely_counter_ai_desc[] = "AI, additive increase";
static const volatile char timely_counter_hai_desc[] = "HAI, hyper-additive increase";
static const volatile char timely_counter_rtt_lt_tlow_desc[] = "RTT_LT_TLOW, instantaneous RTT < T_low";
static const volatile char timely_counter_rtt_gt_thigh_desc[] = "RTT_GT_THIGH, instantaneous RTT > T_high";
static const volatile char timely_counter_rate_clamped_min_desc[] = "RATE_CLAMPED_MIN, rate clamped to min floor";

void timely_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = TIMELY_ALGO_ID;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(timely_desc);
	algo_def.algo_desc_addr = (uint64_t)timely_desc;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def,
					TIMELY_NUM_PARAMS, TIMELY_NUM_COUNTERS);

	/* Per-param description string + size, indexed by TIMELY_P_*. The strings stay
	 * individual volatile globals above (the volatile anchors them against LTO DCE);
	 * the loop reads their addr/size so registration is data-driven. */
	const struct { uint64_t addr; uint32_t size; } param_desc[TIMELY_NUM_PARAMS] = {
		[TIMELY_P_T_LOW_NS]        = {(uint64_t)timely_param_t_low_desc,         sizeof(timely_param_t_low_desc)},
		[TIMELY_P_T_HIGH_NS]       = {(uint64_t)timely_param_t_high_desc,        sizeof(timely_param_t_high_desc)},
		[TIMELY_P_DELTA_FXP20]     = {(uint64_t)timely_param_delta_desc,         sizeof(timely_param_delta_desc)},
		[TIMELY_P_BETA_FXP16]      = {(uint64_t)timely_param_beta_desc,          sizeof(timely_param_beta_desc)},
		[TIMELY_P_ALPHA_FXP16]     = {(uint64_t)timely_param_alpha_desc,         sizeof(timely_param_alpha_desc)},
		[TIMELY_P_MIN_RTT_NS]      = {(uint64_t)timely_param_min_rtt_desc,       sizeof(timely_param_min_rtt_desc)},
		[TIMELY_P_HAI_THRESHOLD]   = {(uint64_t)timely_param_hai_threshold_desc, sizeof(timely_param_hai_threshold_desc)},
		[TIMELY_P_MIN_RATE_FXP20]  = {(uint64_t)timely_param_min_rate_desc,      sizeof(timely_param_min_rate_desc)},
		[TIMELY_P_INIT_RATE_FXP20] = {(uint64_t)timely_param_init_rate_desc,     sizeof(timely_param_init_rate_desc)},
		[TIMELY_P_TRACE_ON]        = {(uint64_t)timely_param_trace_on_desc,      sizeof(timely_param_trace_on_desc)},
	};
	for (uint32_t i = 0; i < TIMELY_NUM_PARAMS; i++) {
		/* Framework min stays loose (1) so the orchestrator owns range validation via
		 * the contract min/max; trace_on alone registers min=0 so it can be toggled
		 * back off at runtime. (Using timely_params[i].min for all would reject in-range
		 * pushes the orchestrator currently allows, e.g. a sub-contract-min beta.) */
		uint32_t reg_min = (i == TIMELY_P_TRACE_ON) ? timely_params[i].min : 1;
		doca_pcc_dev_algo_init_param(algo_idx, i, timely_params[i].def,
					     timely_params[i].max, reg_min, 1,
					     param_desc[i].size, param_desc[i].addr);
	}

	/* Counters all register identically (max=UINT32_MAX, mask=2); only index + desc vary. */
	const struct { uint64_t addr; uint32_t size; } counter_desc[TIMELY_NUM_COUNTERS] = {
		[TIMELY_C_EVENTS]           = {(uint64_t)timely_counter_events_desc,           sizeof(timely_counter_events_desc)},
		[TIMELY_C_MD_GRADIENT]      = {(uint64_t)timely_counter_md_gradient_desc,      sizeof(timely_counter_md_gradient_desc)},
		[TIMELY_C_MD_THIGH]         = {(uint64_t)timely_counter_md_thigh_desc,         sizeof(timely_counter_md_thigh_desc)},
		[TIMELY_C_AI]               = {(uint64_t)timely_counter_ai_desc,               sizeof(timely_counter_ai_desc)},
		[TIMELY_C_HAI]              = {(uint64_t)timely_counter_hai_desc,              sizeof(timely_counter_hai_desc)},
		[TIMELY_C_RTT_LT_TLOW]      = {(uint64_t)timely_counter_rtt_lt_tlow_desc,      sizeof(timely_counter_rtt_lt_tlow_desc)},
		[TIMELY_C_RTT_GT_THIGH]     = {(uint64_t)timely_counter_rtt_gt_thigh_desc,     sizeof(timely_counter_rtt_gt_thigh_desc)},
		[TIMELY_C_RATE_CLAMPED_MIN] = {(uint64_t)timely_counter_rate_clamped_min_desc, sizeof(timely_counter_rate_clamped_min_desc)},
	};
	for (uint32_t i = 0; i < TIMELY_NUM_COUNTERS; i++)
		doca_pcc_dev_algo_init_counter(algo_idx, i, UINT32_MAX, 2,
					       counter_desc[i].size, counter_desc[i].addr);
}

/*
 * Clamp the rate to [min_rate, DOCA_PCC_DEV_MAX_RATE]. Reports the two bounds
 * separately so the caller can drive TIMELY_C_RATE_CLAMPED_MIN distinctly from
 * the trace's combined clamped-bit annotation.
 *
 * @cur_rate [in]: candidate rate (FXP20)
 * @min_rate [in]: minimum allowed rate (FXP20)
 * @clamp_min_flag [out]: set to 1 if rate was below min_rate (may be NULL)
 * @clamp_max_flag [out]: set to 1 if rate exceeded DOCA_PCC_DEV_MAX_RATE (may be NULL)
 * @return: clamped rate (FXP20)
 */
static inline uint32_t timely_clamp_rate(uint32_t cur_rate, uint32_t min_rate,
					 uint32_t *clamp_min_flag,
					 uint32_t *clamp_max_flag)
{
	uint32_t min_hit = 0, max_hit = 0;
	if (cur_rate > DOCA_PCC_DEV_MAX_RATE) {
		cur_rate = DOCA_PCC_DEV_MAX_RATE;
		max_hit = 1;
	}
	if (cur_rate < min_rate) {
		cur_rate = min_rate;
		min_hit = 1;
	}
	if (clamp_min_flag != NULL)
		*clamp_min_flag = min_hit;
	if (clamp_max_flag != NULL)
		*clamp_max_flag = max_hit;
	return cur_rate;
}

/*
 * Compute (T_high / new_rtt) in FXP16 with overflow guard.
 *
 * Used in the multiplicative decrease branch:
 *   rate ← rate · (1 - β · (1 - T_high/new_rtt))
 *
 * FIXME: paper §4.3 does not define overflow handling. If new_rtt ≤ T_high
 * (which shouldn't happen since this branch is only entered when new_rtt > T_high),
 * the ratio would be ≥ 1 and (1 - T_high/new_rtt) would underflow. We clamp to
 * FXP16_ONE - 1 (0xFFFF) to keep the rate in a sane range.
 */
static inline uint32_t timely_t_high_over_rtt_fxp16(uint32_t t_high, uint32_t new_rtt)
{
	if (new_rtt == 0 || new_rtt <= t_high)
		return TIMELY_FXP16_ONE - 1;
	uint64_t ratio = ((uint64_t)t_high << 16) / new_rtt;
	if (ratio >= TIMELY_FXP16_ONE)
		ratio = TIMELY_FXP16_ONE - 1;
	return (uint32_t)ratio;
}

/*
 * Signed FXP16 multiply: (a_fxp16 * b) >> 16, returning int32_t.
 *
 * doca_pcc_dev_fxp_mult is unsigned; the EWMA filter and gradient computations
 * need signed arithmetic because rtt_diff can be negative (queue draining).
 *
 * FIXME: paper Algorithm 1 does not specify how to represent the signed gradient
 * in fixed-point. We use signed 64-bit intermediate arithmetic and rely on the
 * implementation-defined behavior that signed right-shift is arithmetic
 * (sign-extending) on the DPA's Clang compiler. This is true for all practical
 * targets but is technically not guaranteed by C99.
 */
static inline int32_t timely_signed_fxp16_mult(int32_t a_fxp16, int32_t b)
{
	return (int32_t)(((int64_t)a_fxp16 * (int64_t)b) >> 16);
}

/*
 * Handle new flow initialization.
 *
 * Initialize cur_rate to line rate and request the first RTT probe so that
 * subsequent RTT events drive the algorithm.
 *
 * Paper §3 mentions the rate computation engine runs on each completion event;
 * the initial rate is a deployment choice. We start at line rate per common
 * datacenter practice (also matches DCQCN paper §3.1).
 */
static inline void timely_handle_new_flow(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  cc_ctxt_timely_t *ccctx,
					  doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t new_flow_rate = param[TIMELY_P_INIT_RATE_FXP20];

	ccctx->cur_rate = new_flow_rate;
	ccctx->prev_rtt = 0;
	ccctx->rtt_diff = 0;
	ccctx->neg_gradient_count = 0;
	ccctx->start_delay = timestamp;
	ccctx->rtt_meas_psn = 0;
	ccctx->abort_cnt = 0;
	ccctx->rtt_req_to_rtt_sent = 1;

	results->rate = new_flow_rate;
	results->rtt_req = 1;

	TIMELY_TRACE(param, TIMELY_TRACE_FMT_NEW_FLOW, new_flow_rate, timestamp, 0, 0, 0);
	TIMELY_TRACE_FLUSH(param);
}

/*
 * Handle TX event.
 *
 * TIMELY does not adjust the rate on TX events — the paper rate computation
 * engine (§3.2) runs only on completion events. However, we maintain the
 * RTT measurement state machine here (mirroring rtt_template) to detect
 * stuck/aborted probes; without this, a single dropped RTT response would
 * stall the algorithm forever.
 *
 * FIXME: paper §3 assumes the NIC produces RTT measurements via NIC ACK
 * timestamps without explicit probing. In DOCA PCC we explicitly request
 * probes via results->rtt_req. This is a model mapping, not a paper deviation.
 */
static inline void timely_handle_tx(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    cc_ctxt_timely_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	uint8_t rtt_req = 0;
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

	if (unlikely((ev_attr.flags & DOCA_PCC_DEV_TX_FLAG_RTT_REQ_SENT) && (rtt_meas_psn == 0))) {
		ccctx->rtt_meas_psn = 1;
		ccctx->rtt_req_to_rtt_sent = 0;
		ccctx->start_delay = timestamp;
	} else {
		uint32_t rtt_till_now = timestamp - ccctx->start_delay;

		if (unlikely(ccctx->start_delay > timestamp))
			rtt_till_now += UINT32_MAX;

		if (rtt_meas_psn == 0) {
			rtt_till_now = 0;
			ccctx->rtt_req_to_rtt_sent += 1;
		}
		if (unlikely((rtt_till_now > ((uint32_t)TIMELY_ABORT_TIME_NS << ccctx->abort_cnt)) ||
			     (ccctx->rtt_req_to_rtt_sent > 2))) {
			rtt_req = 1;
			if (rtt_till_now > ((uint32_t)TIMELY_ABORT_TIME_NS << ccctx->abort_cnt))
				ccctx->abort_cnt += 1;
			ccctx->rtt_req_to_rtt_sent = 1;
		}
	}

	results->rate = ccctx->cur_rate;
	results->rtt_req = rtt_req;
}

/*
 * Handle RTT event — THE MAIN HANDLER (paper Algorithm 1, §4.3).
 *
 * Computes the new rate based on the smoothed delay gradient and the absolute
 * RTT thresholds T_low / T_high.
 */
static inline void timely_handle_rtt(doca_pcc_dev_event_t *event,
				     uint32_t *param,
				     uint32_t *counter,
				     cc_ctxt_timely_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;

	/* Drop spurious RTT events that are not the one we are waiting for
	 * (e.g., from a previously aborted probe). Mirrors rtt_template. */
	if (unlikely((rtt_meas_psn == 0) && (ccctx->rtt_req_to_rtt_sent == 0))) {
		results->rate = ccctx->cur_rate;
		results->rtt_req = 0;
		return;
	}
	ccctx->rtt_meas_psn = 0;
	ccctx->abort_cnt = 0;

	/* --- RTT calculation (paper §3.1: NIC timestamps for RTT) ---
	 * In DOCA PCC: rtt = response_arrival_ts - request_send_ts.
	 * Without time sync we cannot subtract the NP processing time
	 * (resp_send_ts - req_recv_ts), so the measurement includes a small
	 * but consistent NP-side delay. */
	uint32_t start_rtt = doca_pcc_dev_get_rtt_req_send_timestamp(event);
	uint32_t end_rtt = doca_pcc_dev_get_timestamp(event);
	uint32_t new_rtt = timely_diff_with_wrap32(end_rtt, start_rtt);

	uint32_t t_low = param[TIMELY_P_T_LOW_NS];
	uint32_t t_high = param[TIMELY_P_T_HIGH_NS];
	uint32_t delta = param[TIMELY_P_DELTA_FXP20];
	uint32_t beta = param[TIMELY_P_BETA_FXP16];
	uint32_t alpha = param[TIMELY_P_ALPHA_FXP16];
	uint32_t min_rtt_ns = param[TIMELY_P_MIN_RTT_NS];
	uint32_t hai_threshold = param[TIMELY_P_HAI_THRESHOLD];
	uint32_t min_rate = param[TIMELY_P_MIN_RATE_FXP20];

	uint32_t cur_rate = ccctx->cur_rate;
	int32_t prev_rtt_diff = ccctx->rtt_diff;

	int32_t new_rtt_diff = (int32_t)new_rtt - (int32_t)ccctx->prev_rtt;
	ccctx->prev_rtt = new_rtt;

	/* rtt_diff EWMA = (1-α)·rtt_diff + α·new_rtt_diff. Signed 64-bit intermediates
	 * so a negative new_rtt_diff (queue draining) stays negative. */
	int32_t one_minus_alpha = (int32_t)(TIMELY_FXP16_ONE - alpha);
	int32_t term_old = timely_signed_fxp16_mult(one_minus_alpha, prev_rtt_diff);
	int32_t term_new = timely_signed_fxp16_mult((int32_t)alpha, new_rtt_diff);
	int32_t rtt_diff = term_old + term_new;
	ccctx->rtt_diff = rtt_diff;

	/* normalized_gradient = rtt_diff / minRTT */
	int32_t normalized_gradient = 0;
	if (likely(min_rtt_ns != 0))
		normalized_gradient = rtt_diff / (int32_t)min_rtt_ns;

	uint32_t branch_taken = 0;

	/* T_low: additive increase. Uses INSTANTANEOUS new_rtt (paper §4.3 "need for RTT
	 * low threshold" — filters burst spikes), not the smoothed value. */
	if (new_rtt < t_low) {
		cur_rate += delta;
		ccctx->neg_gradient_count = 0;
		branch_taken = TIMELY_BRANCH_T_LOW;
		if (counter != NULL) {
			counter[TIMELY_C_RTT_LT_TLOW]++;
			counter[TIMELY_C_AI]++;
		}
		goto done;
	}

	/* T_high: multiplicative decrease, rate ← rate·(1 - β·(1 - T_high/new_rtt)).
	 * Paper §4.3 decides on INSTANTANEOUS new_rtt here, not the smoothed value. */
	if (new_rtt > t_high) {
		uint32_t ratio_fxp16 = timely_t_high_over_rtt_fxp16(t_high, new_rtt);
		uint32_t one_minus_ratio = TIMELY_FXP16_ONE - ratio_fxp16;
		uint32_t beta_term = doca_pcc_dev_fxp_mult(beta, one_minus_ratio);
		/* β · (1 - T_high/new_rtt) ≤ β < 1, so factor ≥ 1 - β > 0 */
		uint32_t factor_fxp16 = TIMELY_FXP16_ONE - beta_term;
		cur_rate = doca_pcc_dev_fxp_mult(factor_fxp16, cur_rate);
		ccctx->neg_gradient_count = 0;
		branch_taken = TIMELY_BRANCH_T_HIGH;
		if (counter != NULL) {
			counter[TIMELY_C_RTT_GT_THIGH]++;
			counter[TIMELY_C_MD_THIGH]++;
		}
		goto done;
	}

	/* gradient ≤ 0 (queue draining or stable): additive, or HAI after a neg streak */
	if (normalized_gradient <= 0) {
		uint16_t neg_count = ccctx->neg_gradient_count;
		if (neg_count < (uint16_t)hai_threshold)
			neg_count++;
		ccctx->neg_gradient_count = neg_count;

		uint32_t n_factor = (neg_count >= (uint16_t)hai_threshold) ? TIMELY_HAI_FACTOR : 1;
		cur_rate += n_factor * delta;
		branch_taken = TIMELY_BRANCH_GRAD_LE;
		if (counter != NULL) {
			if (n_factor != 1)
				counter[TIMELY_C_HAI]++;
			else
				counter[TIMELY_C_AI]++;
		}

		if (n_factor != 1)
			TIMELY_TRACE(param, TIMELY_TRACE_FMT_HAI, neg_count, n_factor, cur_rate, delta, 0);
		goto done;
	}

	/* gradient > 0: multiplicative decrease, rate ← rate·(1 - β·normalized_gradient).
	 * rtt_diff/min_rtt_ns are ns; the FXP16 ratio is (rtt_diff<<16)/min_rtt_ns,
	 * saturated at FXP16_ONE-1 so (1 - β·ratio) stays positive on large gradients.
	 * FIXME: paper doesn't bound this multiplier; saturation is our choice. */
	{
		int64_t grad_fxp16_signed = ((int64_t)rtt_diff << 16) / (int32_t)min_rtt_ns;
		uint32_t grad_fxp16 = (grad_fxp16_signed >= TIMELY_FXP16_ONE)
					      ? (uint32_t)(TIMELY_FXP16_ONE - 1)
					      : (uint32_t)grad_fxp16_signed;

		uint32_t beta_term = doca_pcc_dev_fxp_mult(beta, grad_fxp16);
		if (beta_term >= TIMELY_FXP16_ONE)
			beta_term = TIMELY_FXP16_ONE - 1;
		uint32_t factor_fxp16 = TIMELY_FXP16_ONE - beta_term;
		cur_rate = doca_pcc_dev_fxp_mult(factor_fxp16, cur_rate);
		ccctx->neg_gradient_count = 0;
		branch_taken = TIMELY_BRANCH_GRAD_GT;
		if (counter != NULL)
			counter[TIMELY_C_MD_GRADIENT]++;
	}

done:;
	/* Paper §5 scales rate updates by (completions per minRTT) because their algorithm
	 * runs on every data-segment ACK (high cadence at small segment sizes). DOCA PCC
	 * decouples our handler from data ACKs: it runs only on RTT events driven by
	 * dedicated probes (requested via results->rtt_req). Empirically, probe inter-arrival
	 * on our setup is ~16 ms ≫ minRTT (~µs), i.e. far fewer than 1 update per minRTT.
	 * Paper's "≤1 completion per minRTT" assumption holds → no scaling needed. */

	uint32_t clamp_min = 0, clamp_max = 0;
	cur_rate = timely_clamp_rate(cur_rate, min_rate, &clamp_min, &clamp_max);
	if (clamp_min || clamp_max)
		branch_taken |= TIMELY_BRANCH_CLAMPED_BIT;
	if (clamp_min && counter != NULL)
		counter[TIMELY_C_RATE_CLAMPED_MIN]++;
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 1;
	ccctx->rtt_req_to_rtt_sent = 1;

	TIMELY_TRACE(param, TIMELY_TRACE_FMT_RTT, new_rtt, (uint32_t)rtt_diff, (uint32_t)normalized_gradient,
		     branch_taken, cur_rate);
	TIMELY_TRACE_FLUSH(param);
}

/*
 * Handle NACK event.
 *
 * FIXME: paper does not define NACK handling for TIMELY (paper §5: "we use
 * OS-bypass messaging over a fabric with PFC", i.e., a PFC-protected fabric
 * where packet loss should be rare). Apply a conservative 50% rate cut and
 * reset the negative-gradient counter to drop out of any pending HAI mode.
 */
static inline void timely_handle_nack(doca_pcc_dev_event_t *event,
				      uint32_t *param,
				      cc_ctxt_timely_t *ccctx,
				      doca_pcc_dev_results_t *results)
{
	uint32_t cur_rate = ccctx->cur_rate >> 1;

	cur_rate = timely_clamp_rate(cur_rate, param[TIMELY_P_MIN_RATE_FXP20], NULL, NULL);
	ccctx->cur_rate = cur_rate;
	ccctx->neg_gradient_count = 0;
	results->rate = cur_rate;
	results->rtt_req = 0;
}

/*
 * Handle CNP event.
 *
 * FIXME: paper §1 explicitly states TIMELY does NOT use ECN/CNP. If a CNP
 * arrives anyway (e.g., the fabric still generates CNPs for legacy reasons),
 * we ignore it and pass through the current rate. The rate will adjust on
 * the next RTT event based on the actual delay.
 */
static inline void timely_handle_cnp(doca_pcc_dev_event_t *event,
				     uint32_t *param,
				     cc_ctxt_timely_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	results->rate = ccctx->cur_rate;
	results->rtt_req = 0;
}

void timely_algo(doca_pcc_dev_event_t *event,
		 uint32_t *param,
		 uint32_t *counter,
		 doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		 doca_pcc_dev_results_t *results)
{
	cc_ctxt_timely_t *timely_ctx = (cc_ctxt_timely_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = timely_ctx->cur_rate;

	if (counter != NULL)
		counter[TIMELY_C_EVENTS]++;

	if (unlikely(cur_rate == 0)) {
		timely_handle_new_flow(event, param, timely_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_RTT) {
		timely_handle_rtt(event, param, counter, timely_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		timely_handle_tx(event, param, timely_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		timely_handle_nack(event, param, timely_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		timely_handle_cnp(event, param, timely_ctx, results);
	} else {
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t timely_set_algo_params(uint32_t param_id_base,
					    uint32_t param_num,
					    const uint32_t *new_param_values,
					    uint32_t *params)
{
	/* Reject writes that would run off the end of the param array. param_id_base
	 * and param_num are bounded individually AND together: param_id_base+param_num
	 * must stay within TIMELY_NUM_PARAMS, otherwise the copy below writes OOB. */
	if ((param_id_base >= TIMELY_NUM_PARAMS) ||
	    (param_num > TIMELY_NUM_PARAMS) ||
	    (param_id_base + param_num > TIMELY_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* Commit the validated values into the live param array. The framework does
	 * NOT auto-copy: per the doca_pcc_dev_user_set_algo_params contract, returning
	 * OK means "the callback set them" (FAIL means "nothing changed"). Without this
	 * loop, the array returned by doca_pcc_dev_get_algo_params() keeps the init-time
	 * defaults, so PPCC writes read back correctly (register image) yet never reach
	 * the running algorithm. Range is already enforced by the min/max registered in
	 * timely_init(), so no per-field re-validation is needed here. */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
