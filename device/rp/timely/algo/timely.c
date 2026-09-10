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
 * TIMELY: RTT-based congestion control, RP (Reaction Point) side.
 *
 * Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter",
 * SIGCOMM 2015. Algorithm 1 (paper Section 4.3), parameters (paper Section 6).
 *
 * The controller is rate-based and RTT-driven: on every RTT event it adjusts the
 * per-flow rate from the smoothed delay gradient. RTT is the only congestion
 * signal; ECN is not used.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "timely_ctxt.h"
#include "timely_params.h"
#include "timely.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Per-event trace emission and buffer flush, gated by the trace_on PPCC param. */
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

/* Trace format IDs, matched by the host-side format table. */
#define TIMELY_TRACE_FMT_NEW_FLOW (11)
#define TIMELY_TRACE_FMT_RTT      (12)
#define TIMELY_TRACE_FMT_HAI      (14)

/* Branch identifiers in the format-12 trace: low 4 bits = the Algorithm 1 branch
 * taken, bit 7 (0x80) = the rate was clamped to MIN_RATE or DOCA_PCC_DEV_MAX_RATE. */
#define TIMELY_BRANCH_T_LOW       (0)
#define TIMELY_BRANCH_T_HIGH      (1)
#define TIMELY_BRANCH_GRAD_LE     (2)
#define TIMELY_BRANCH_GRAD_GT     (3)
#define TIMELY_BRANCH_CLAMPED_BIT (0x80)

/* Base timeout for a stuck RTT request, in ns. */
#define TIMELY_ABORT_TIME_NS (300000)

/* FXP16 1.0, and the HAI increase multiplier N = 5 (paper Section 4.3). */
#define TIMELY_FXP16_ONE (1 << 16)
#define TIMELY_HAI_FACTOR (5)

/*
 * Difference between two 32-bit timestamps, wrapping correctly on overflow.
 * Local copy: the pcc_common_dev.h version is ALWAYS_INLINE and does not survive
 * -flto when included in two translation units.
 */
static inline uint32_t timely_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* PPCC description strings; volatile keeps LTO from eliminating them. */
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
static const volatile char timely_counter_rate_clamped_max_desc[] = "RATE_CLAMPED_MAX, rate clamped to line-rate ceiling";

/* Register TIMELY's metadata, parameters and counters (contract in timely_params.h). */
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

	/* Register every parameter from the contract table with def/max from
	 * timely_params[] and a framework min hardcoded to 1: the table minima are tuning
	 * guidance, not framework-legal bounds, and one value the firmware rejects reverts
	 * the whole bulk PPCC vector to defaults. */
#define TIMELY_REG_PARAM(idx, desc) \
	doca_pcc_dev_algo_init_param(algo_idx, (idx), timely_params[(idx)].def, timely_params[(idx)].max, \
				     1, 1, sizeof(desc), (uint64_t)(desc))

	TIMELY_REG_PARAM(TIMELY_P_T_LOW_NS,        timely_param_t_low_desc);
	TIMELY_REG_PARAM(TIMELY_P_T_HIGH_NS,       timely_param_t_high_desc);
	TIMELY_REG_PARAM(TIMELY_P_DELTA_FXP20,     timely_param_delta_desc);
	TIMELY_REG_PARAM(TIMELY_P_BETA_FXP16,      timely_param_beta_desc);
	TIMELY_REG_PARAM(TIMELY_P_ALPHA_FXP16,     timely_param_alpha_desc);
	TIMELY_REG_PARAM(TIMELY_P_MIN_RTT_NS,      timely_param_min_rtt_desc);
	TIMELY_REG_PARAM(TIMELY_P_HAI_THRESHOLD,   timely_param_hai_threshold_desc);
	TIMELY_REG_PARAM(TIMELY_P_MIN_RATE_FXP20,  timely_param_min_rate_desc);
	TIMELY_REG_PARAM(TIMELY_P_INIT_RATE_FXP20, timely_param_init_rate_desc);
#undef TIMELY_REG_PARAM

	/* trace_on registers the table min (0) so traces can be switched off at runtime. */
	doca_pcc_dev_algo_init_param(algo_idx, TIMELY_P_TRACE_ON,
				     timely_params[TIMELY_P_TRACE_ON].def,
				     timely_params[TIMELY_P_TRACE_ON].max,
				     timely_params[TIMELY_P_TRACE_ON].min, 1,
				     sizeof(timely_param_trace_on_desc),
				     (uint64_t)timely_param_trace_on_desc);

	/* Counters register identically; only index and description vary. */
#define TIMELY_REG_COUNTER(idx, desc) \
	doca_pcc_dev_algo_init_counter(algo_idx, (idx), UINT32_MAX, 2, sizeof(desc), (uint64_t)(desc))

	TIMELY_REG_COUNTER(TIMELY_C_EVENTS,           timely_counter_events_desc);
	TIMELY_REG_COUNTER(TIMELY_C_MD_GRADIENT,      timely_counter_md_gradient_desc);
	TIMELY_REG_COUNTER(TIMELY_C_MD_THIGH,         timely_counter_md_thigh_desc);
	TIMELY_REG_COUNTER(TIMELY_C_AI,               timely_counter_ai_desc);
	TIMELY_REG_COUNTER(TIMELY_C_HAI,              timely_counter_hai_desc);
	TIMELY_REG_COUNTER(TIMELY_C_RTT_LT_TLOW,      timely_counter_rtt_lt_tlow_desc);
	TIMELY_REG_COUNTER(TIMELY_C_RTT_GT_THIGH,     timely_counter_rtt_gt_thigh_desc);
	TIMELY_REG_COUNTER(TIMELY_C_RATE_CLAMPED_MIN, timely_counter_rate_clamped_min_desc);
	TIMELY_REG_COUNTER(TIMELY_C_RATE_CLAMPED_MAX, timely_counter_rate_clamped_max_desc);
#undef TIMELY_REG_COUNTER
}

/*
 * Clamp the rate to [min_rate, DOCA_PCC_DEV_MAX_RATE], reporting each bound
 * separately so the caller can count the two saturations apart.
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
 * Compute T_high / new_rtt in FXP16 for the multiplicative decrease
 * rate = rate * (1 - beta * (1 - T_high/new_rtt)) (paper Section 4.3).
 * Saturated at FXP16_ONE - 1 so (1 - T_high/new_rtt) cannot underflow.
 *
 * @t_high [in]: RTT high threshold (ns)
 * @new_rtt [in]: RTT sample (ns)
 * @return: ratio in FXP16, at most FXP16_ONE - 1
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
 * Signed FXP16 multiply: (a_fxp16 * b) >> 16. Needed because
 * doca_pcc_dev_fxp_mult is unsigned while rtt_diff can be negative.
 *
 * @a_fxp16 [in]: multiplicand in FXP16
 * @b [in]: signed multiplier
 * @return: product with the FXP16 scaling removed
 */
static inline int32_t timely_signed_fxp16_mult(int32_t a_fxp16, int32_t b)
{
	return (int32_t)(((int64_t)a_fxp16 * (int64_t)b) >> 16);
}

/*
 * Multiplicative decrease: rate = rate * (1 - beta * ratio_fxp16).
 * beta_term is saturated below 1.0 so the factor stays positive on large ratios.
 *
 * @cur_rate [in]: current rate (FXP20)
 * @beta [in]: multiplicative decrease factor (FXP16)
 * @ratio_fxp16 [in]: decrease ratio (FXP16)
 * @return: decreased rate (FXP20)
 */
static inline uint32_t timely_apply_md(uint32_t cur_rate, uint32_t beta, uint32_t ratio_fxp16)
{
	uint32_t beta_term = doca_pcc_dev_fxp_mult(beta, ratio_fxp16);
	if (beta_term >= TIMELY_FXP16_ONE)
		beta_term = TIMELY_FXP16_ONE - 1;
	uint32_t factor_fxp16 = TIMELY_FXP16_ONE - beta_term;
	return doca_pcc_dev_fxp_mult(factor_fxp16, cur_rate);
}

/*
 * Initialize a new flow: set the rate to init_rate and request the first RTT
 * probe, so that subsequent RTT events drive the algorithm.
 *
 * @event [in]: the event that created the flow
 * @param [in]: PPCC parameter array
 * @ccctx [out]: per-flow context to initialize
 * @results [out]: rate and rtt_req returned to the hardware
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
	ccctx->probe_start_ts = timestamp;
	ccctx->probe_in_flight = 0;
	ccctx->abort_cnt = 0;
	ccctx->tx_since_probe_req = 1;

	results->rate = new_flow_rate;
	results->rtt_req = 1;

	TIMELY_TRACE(param, TIMELY_TRACE_FMT_NEW_FLOW,
		     TIMELY_TRACE_TSFLOW(timestamp, doca_pcc_dev_get_flowtag(event)),
		     new_flow_rate, 0, 0, 0);
	TIMELY_TRACE_FLUSH(param);
}

/*
 * Handle a TX event. The rate is left unchanged: the rate computation engine
 * runs on RTT events only (paper Section 3.2). This drives the RTT measurement
 * state machine, re-requesting a probe whose response never arrived.
 *
 * @event [in]: TX event
 * @param [in]: PPCC parameter array
 * @ccctx [in/out]: per-flow context
 * @results [out]: rate and rtt_req returned to the hardware
 */
static inline void timely_handle_tx(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    cc_ctxt_timely_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	uint8_t rtt_req = 0;
	uint32_t probe_in_flight = ccctx->probe_in_flight;
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

	if (unlikely((ev_attr.flags & DOCA_PCC_DEV_TX_FLAG_RTT_REQ_SENT) && (probe_in_flight == 0))) {
		ccctx->probe_in_flight = 1;
		ccctx->tx_since_probe_req = 0;
		ccctx->probe_start_ts = timestamp;
	} else {
		uint32_t probe_out_time_ns = timestamp - ccctx->probe_start_ts;

		if (unlikely(ccctx->probe_start_ts > timestamp))
			probe_out_time_ns += UINT32_MAX;

		if (probe_in_flight == 0) {
			probe_out_time_ns = 0;
			ccctx->tx_since_probe_req += 1;
		}
		/* Clamp the backoff exponent at use and at growth: a shift >= 32 is UB. */
		uint32_t abort_shift = (ccctx->abort_cnt < 12) ? ccctx->abort_cnt : 12;
		uint32_t abort_threshold = (uint32_t)TIMELY_ABORT_TIME_NS << abort_shift;
		if (unlikely((probe_out_time_ns > abort_threshold) ||
			     (ccctx->tx_since_probe_req > 2))) {
			rtt_req = 1;
			if ((probe_out_time_ns > abort_threshold) && (ccctx->abort_cnt < 12))
				ccctx->abort_cnt += 1;
			ccctx->tx_since_probe_req = 1;
		}
	}

	results->rate = ccctx->cur_rate;
	results->rtt_req = rtt_req;
}

/*
 * Handle an RTT event: run Algorithm 1 (paper Section 4.3), computing the new
 * rate from the smoothed delay gradient and the RTT thresholds T_low / T_high.
 *
 * @event [in]: RTT event
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ccctx [in/out]: per-flow context
 * @results [out]: rate and rtt_req returned to the hardware
 */
static inline void timely_handle_rtt(doca_pcc_dev_event_t *event,
				     uint32_t *param,
				     uint32_t *counter,
				     cc_ctxt_timely_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	uint32_t probe_in_flight = ccctx->probe_in_flight;

	/* Drop a spurious RTT event, such as the response to an already aborted probe. */
	if (unlikely((probe_in_flight == 0) && (ccctx->tx_since_probe_req == 0))) {
		results->rate = ccctx->cur_rate;
		results->rtt_req = 0;
		return;
	}
	ccctx->probe_in_flight = 0;
	ccctx->abort_cnt = 0;

	/* RTT sample = response arrival - request send (paper Section 3.1). The NP-side
	 * processing time is included: it cannot be subtracted without time sync. */
	uint32_t req_send_ts = doca_pcc_dev_get_rtt_req_send_timestamp(event);
	uint32_t resp_recv_ts = doca_pcc_dev_get_timestamp(event);
	uint32_t new_rtt = timely_diff_with_wrap32(resp_recv_ts, req_send_ts);

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

	/* First sample on a flow: seed prev_rtt so the first diff is 0 rather than the
	 * full RTT, which would read as a large increase and cut the rate. */
	if (unlikely(ccctx->prev_rtt == 0))
		ccctx->prev_rtt = new_rtt;

	int32_t new_rtt_diff = (int32_t)new_rtt - (int32_t)ccctx->prev_rtt;
	ccctx->prev_rtt = new_rtt;

	/* rtt_diff EWMA = (1-alpha)*rtt_diff + alpha*new_rtt_diff (Algorithm 1), signed so
	 * that a negative new_rtt_diff (queue draining) stays negative. */
	int32_t one_minus_alpha = (int32_t)(TIMELY_FXP16_ONE - alpha);
	int32_t term_old = timely_signed_fxp16_mult(one_minus_alpha, prev_rtt_diff);
	int32_t term_new = timely_signed_fxp16_mult((int32_t)alpha, new_rtt_diff);
	int32_t rtt_diff = term_old + term_new;
	ccctx->rtt_diff = rtt_diff;

	/* normalized_gradient = rtt_diff / minRTT in FXP16 (1<<16 = 1.0). The FXP16 scale
	 * keeps the quotient from truncating to 0 on every event, which would disable both
	 * the sign test and the HAI streak. Saturated into int32: an overflowing quotient
	 * would flip the sign and read a large RTT increase as a decrease. */
	int32_t normalized_gradient = 0;
	if (likely(min_rtt_ns != 0)) {
		int64_t grad = ((int64_t)rtt_diff << 16) / (int32_t)min_rtt_ns;

		if (grad > INT32_MAX)
			grad = INT32_MAX;
		else if (grad < INT32_MIN)
			grad = INT32_MIN;
		normalized_gradient = (int32_t)grad;
	}

	/* HAI streak: only a strictly negative gradient extends it, a zero or positive one
	 * breaks it (Algorithm 1: N = 5 after five such events). Updated ahead of the
	 * branch chain so that samples outside the gradient zone, in particular those
	 * below T_low, keep the streak alive. Saturated at hai_threshold, since only the
	 * >= test matters and the counter must not wrap on a long negative run. */
	uint16_t neg_count = ccctx->neg_gradient_count;
	if (normalized_gradient < 0) {
		if (neg_count < (uint16_t)hai_threshold)
			neg_count++;
	} else {
		neg_count = 0;
	}
	ccctx->neg_gradient_count = neg_count;

	uint32_t branch_taken = 0;

	/* T_low: additive increase, decided on the instantaneous new_rtt rather than the
	 * smoothed value (paper Section 4.3). */
	if (new_rtt < t_low) {
		cur_rate += delta;
		branch_taken = TIMELY_BRANCH_T_LOW;
		if (counter != NULL) {
			counter[TIMELY_C_RTT_LT_TLOW]++;
			counter[TIMELY_C_AI]++;
		}
		goto done;
	}

	/* T_high: multiplicative decrease, rate = rate * (1 - beta * (1 - T_high/new_rtt)),
	 * also decided on the instantaneous new_rtt (paper Section 4.3). */
	if (new_rtt > t_high) {
		uint32_t one_minus_ratio = TIMELY_FXP16_ONE - timely_t_high_over_rtt_fxp16(t_high, new_rtt);
		cur_rate = timely_apply_md(cur_rate, beta, one_minus_ratio);
		branch_taken = TIMELY_BRANCH_T_HIGH;
		if (counter != NULL) {
			counter[TIMELY_C_RTT_GT_THIGH]++;
			counter[TIMELY_C_MD_THIGH]++;
		}
		goto done;
	}

	/* gradient <= 0 (queue draining or stable): additive increase, or hyper-additive
	 * once the streak has reached hai_threshold. */
	if (normalized_gradient <= 0) {
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
			TIMELY_TRACE(param, TIMELY_TRACE_FMT_HAI,
				     TIMELY_TRACE_TSFLOW(resp_recv_ts, doca_pcc_dev_get_flowtag(event)),
				     neg_count, n_factor, delta, cur_rate);
		goto done;
	}

	/* gradient > 0: multiplicative decrease, rate = rate * (1 - beta * gradient). The
	 * gradient is already an FXP16 ratio, saturated at FXP16_ONE - 1 so the factor
	 * stays positive on large gradients. */
	{
		uint32_t grad_fxp16 = (normalized_gradient >= (int32_t)TIMELY_FXP16_ONE)
					      ? (uint32_t)(TIMELY_FXP16_ONE - 1)
					      : (uint32_t)normalized_gradient;
		cur_rate = timely_apply_md(cur_rate, beta, grad_fxp16);
		branch_taken = TIMELY_BRANCH_GRAD_GT;
		if (counter != NULL)
			counter[TIMELY_C_MD_GRADIENT]++;
	}

done:;
	uint32_t clamp_min = 0, clamp_max = 0;
	cur_rate = timely_clamp_rate(cur_rate, min_rate, &clamp_min, &clamp_max);
	if (clamp_min || clamp_max)
		branch_taken |= TIMELY_BRANCH_CLAMPED_BIT;
	if (counter != NULL) {
		if (clamp_min)
			counter[TIMELY_C_RATE_CLAMPED_MIN]++;
		if (clamp_max)
			counter[TIMELY_C_RATE_CLAMPED_MAX]++;
	}
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 1;
	ccctx->tx_since_probe_req = 1;

	TIMELY_TRACE(param, TIMELY_TRACE_FMT_RTT,
		     TIMELY_TRACE_TSFLOW(resp_recv_ts, doca_pcc_dev_get_flowtag(event)),
		     new_rtt, (uint64_t)(int64_t)(int32_t)(normalized_gradient),
		     branch_taken, cur_rate);
	TIMELY_TRACE_FLUSH(param);
}

/* Per-event entry point: dispatch to the new-flow, RTT or TX handler (declared in timely.h). */
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
	} else {
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

/* Apply a PPCC parameter write to the live param array (declared in timely.h). */
doca_pcc_dev_error_t timely_set_algo_params(uint32_t param_id_base,
					    uint32_t param_num,
					    const uint32_t *new_param_values,
					    uint32_t *params)
{
	/* Reject a write that would run off the end of the param array. */
	if ((param_id_base >= TIMELY_NUM_PARAMS) ||
	    (param_num > TIMELY_NUM_PARAMS) ||
	    (param_id_base + param_num > TIMELY_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* The framework does not copy the new values; returning OK means this callback
	 * set them. Range is already enforced by the min/max registered in timely_init(). */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
