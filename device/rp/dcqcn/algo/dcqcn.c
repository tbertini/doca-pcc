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
 * DCQCN: Datacenter QCN congestion control algorithm
 *
 * Implementation of the RP (Reaction Point) algorithm from:
 * Zhu et al., "Congestion Control for Large-Scale RDMA Deployments", SIGCOMM 2015.
 *
 * Reference: Equations (1)-(4) and Figure 7 of the paper.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "dcqcn_ctxt.h"
#include "dcqcn_algo_params.h"
#include "dcqcn.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Define DCQCN_DEBUG at compile time to enable detailed tracing.
 * WARNING: significant performance overhead on hot path (every TX event). */
#ifdef DCQCN_DEBUG
#define DCQCN_TRACE(fmt_id, a1, a2, a3, a4, a5) \
	doca_pcc_dev_trace_5(fmt_id, (uint64_t)(a1), (uint64_t)(a2), (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5))
#define DCQCN_TRACE_FLUSH() doca_pcc_dev_trace_flush()
#else
#define DCQCN_TRACE(fmt_id, a1, a2, a3, a4, a5) do {} while (0)
#define DCQCN_TRACE_FLUSH() do {} while (0)
#endif

/* Debug trace format IDs (only active when DCQCN_DEBUG is defined) */
#define DCQCN_TRACE_FMT_NEW_FLOW  (7)
#define DCQCN_TRACE_FMT_CNP       (8)
#define DCQCN_TRACE_FMT_TX        (9)
#define DCQCN_TRACE_FMT_RATE_INC  (10)

/*
 * Calculate wrap around in case of int overflow.
 * Local static copy to avoid duplicate symbol with pcc_common_dev.h under LTO.
 */
static inline uint32_t dcqcn_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* Bound on alpha-decay catch-up iterations per event (keeps the loop finite on the
 * DPA). 32 decays at g=1/256 only reduce α by (255/256)^32 ≈ 0.88, so a gap longer
 * than the bound means a long-idle flow: α snaps to 0 instead of iterating. */
#define DCQCN_MAX_ALPHA_CATCHUP (32)

#define DCQCN_MAX_TIMER_CATCHUP (8)

typedef enum {
	DCQCN_PARAM_TIMER_PERIOD = 0,	  /* Timer period T in nanoseconds */
	DCQCN_PARAM_BYTE_COUNTER_TH = 1, /* Byte counter threshold B in bytes */
	DCQCN_PARAM_G_SHIFT = 2,	  /* g = 1/(1 << G_SHIFT) */
	DCQCN_PARAM_F_THRESHOLD = 3,	  /* Fast recovery threshold F */
	DCQCN_PARAM_R_AI = 4,		  /* Additive increase step R_AI (FXP20) */
	DCQCN_PARAM_R_HAI = 5,		  /* Hyper additive increase step R_HAI (FXP20) */
	DCQCN_PARAM_NEW_FLOW_RATE = 6,	  /* New flow initial rate (FXP20) */
	DCQCN_PARAM_MIN_RATE = 7,	  /* Minimum rate floor (FXP20) */
	DCQCN_PARAM_NUM		  /* Total number of configurable parameters */
} dcqcn_params_t;

typedef enum {
	DCQCN_COUNTER_TX_EVENT = 0,
	DCQCN_COUNTER_CNP_EVENT = 1,
	DCQCN_COUNTER_NACK_EVENT = 2,
	DCQCN_COUNTER_NUM
} dcqcn_counter_t;

const volatile char dcqcn_desc[] = "DCQCN v1.0 (Zhu et al. SIGCOMM 2015)";
static const volatile char dcqcn_param_timer_period_desc[] = "TIMER_PERIOD, rate increase timer period in ns";
static const volatile char dcqcn_param_byte_counter_th_desc[] = "BYTE_COUNTER_TH, byte counter threshold in bytes";
static const volatile char dcqcn_param_g_shift_desc[] = "G_SHIFT, alpha EWMA weight as shift (g=1/(1<<G_SHIFT))";
static const volatile char dcqcn_param_f_threshold_desc[] = "F_THRESHOLD, fast recovery step threshold";
static const volatile char dcqcn_param_r_ai_desc[] = "R_AI, additive increase step (FXP20)";
static const volatile char dcqcn_param_r_hai_desc[] = "R_HAI, hyper additive increase step (FXP20)";
static const volatile char dcqcn_param_new_flow_rate_desc[] = "NEW_FLOW_RATE, initial rate for new flows (FXP20)";
static const volatile char dcqcn_param_min_rate_desc[] = "MIN_RATE, minimum rate floor (FXP20)";
static const volatile char dcqcn_counter_tx_desc[] = "COUNTER_TX_EVENT, number of TX events handled";
static const volatile char dcqcn_counter_cnp_desc[] = "COUNTER_CNP_EVENT, number of CNP events handled";
static const volatile char dcqcn_counter_nack_desc[] = "COUNTER_NACK_EVENT, number of NACK events handled";

void dcqcn_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = 0xBFFE;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(dcqcn_desc);
	algo_def.algo_desc_addr = (uint64_t)dcqcn_desc;

	uint32_t total_param_num = DCQCN_PARAM_NUM;
	uint32_t total_counter_num = DCQCN_COUNTER_NUM;
	uint32_t param_num = 0;
	uint32_t counter_num = 0;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, total_param_num, total_counter_num);

	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_TIMER_PERIOD_NS,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_timer_period_desc),
				     (uint64_t)dcqcn_param_timer_period_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_BYTE_COUNTER_TH,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_byte_counter_th_desc),
				     (uint64_t)dcqcn_param_byte_counter_th_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_G_SHIFT,
				     16,
				     1,
				     1,
				     sizeof(dcqcn_param_g_shift_desc),
				     (uint64_t)dcqcn_param_g_shift_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_F_THRESHOLD,
				     100,
				     1,
				     1,
				     sizeof(dcqcn_param_f_threshold_desc),
				     (uint64_t)dcqcn_param_f_threshold_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_R_AI,
				     DCQCN_RATE_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_r_ai_desc),
				     (uint64_t)dcqcn_param_r_ai_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_R_HAI,
				     DCQCN_RATE_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_r_hai_desc),
				     (uint64_t)dcqcn_param_r_hai_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_NEW_FLOW_RATE,
				     DCQCN_RATE_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_new_flow_rate_desc),
				     (uint64_t)dcqcn_param_new_flow_rate_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     DCQCN_MIN_RATE,
				     DCQCN_RATE_MAX,
				     1,
				     1,
				     sizeof(dcqcn_param_min_rate_desc),
				     (uint64_t)dcqcn_param_min_rate_desc);

	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(dcqcn_counter_tx_desc),
				       (uint64_t)dcqcn_counter_tx_desc);
	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(dcqcn_counter_cnp_desc),
				       (uint64_t)dcqcn_counter_cnp_desc);
	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(dcqcn_counter_nack_desc),
				       (uint64_t)dcqcn_counter_nack_desc);
}

/*
 * Rate increase logic implementing the three phases from Figure 7 of the paper.
 *
 * Phase determination (paper Section 3.1):
 *   - Fast Recovery:     max(T, BC) < F  =>  RC = (RT + RC) / 2
 *   - Additive Increase: min(T, BC) > F  =>  RT += R_AI;  RC = (RT + RC) / 2
 *   - Hyper Increase:    otherwise        =>  RT += R_HAI; RC = (RT + RC) / 2
 *
 * @ccctx [in/out]: Per-flow context
 * @cur_rate [in]: Current rate RC (FXP20)
 * @param [in]: Algorithm parameters
 * @return: Updated rate RC (FXP20)
 */
static inline uint32_t dcqcn_rate_increase(cc_ctxt_dcqcn_t *ccctx,
					   uint32_t cur_rate,
					   uint32_t *param)
{
	uint16_t t_count = ccctx->t_count;
	uint16_t bc_count = ccctx->bc_count;
	uint32_t target_rate = ccctx->target_rate;
#ifdef DCQCN_DEBUG
	uint32_t old_rate = cur_rate;
#endif
	uint32_t F = param[DCQCN_PARAM_F_THRESHOLD];
	uint32_t R_AI = param[DCQCN_PARAM_R_AI];
	uint32_t R_HAI = param[DCQCN_PARAM_R_HAI];

	uint16_t max_count = (t_count > bc_count) ? t_count : bc_count;
	uint16_t min_count = (t_count < bc_count) ? t_count : bc_count;

	if (max_count < F) {
		/* Fast Recovery (paper Equation 3): RC = (RT + RC) / 2 */
		cur_rate = (target_rate + cur_rate) >> 1;
		DCQCN_TRACE(DCQCN_TRACE_FMT_RATE_INC, old_rate, cur_rate, target_rate, 0 /* FR */, ((uint32_t)t_count << 16) | bc_count);
	} else if (min_count > F) {
		/* Additive Increase (paper Equation 4): RT += R_AI; RC = (RT + RC) / 2 */
		target_rate += R_AI;
		if (target_rate > DOCA_PCC_DEV_MAX_RATE)
			target_rate = DOCA_PCC_DEV_MAX_RATE;
		cur_rate = (target_rate + cur_rate) >> 1;
		DCQCN_TRACE(DCQCN_TRACE_FMT_RATE_INC, old_rate, cur_rate, target_rate, 2 /* AI */, ((uint32_t)t_count << 16) | bc_count);
	} else {
		/* Hyper Increase (paper Section 3.1, QCN [17]): RT += R_HAI; RC = (RT + RC) / 2 */
		target_rate += R_HAI;
		if (target_rate > DOCA_PCC_DEV_MAX_RATE)
			target_rate = DOCA_PCC_DEV_MAX_RATE;
		cur_rate = (target_rate + cur_rate) >> 1;
		DCQCN_TRACE(DCQCN_TRACE_FMT_RATE_INC, old_rate, cur_rate, target_rate, 1 /* HI */, ((uint32_t)t_count << 16) | bc_count);
	}

	ccctx->target_rate = target_rate;
	return cur_rate;
}

/*
 * Alpha decay: α = (1 - g) * α  (paper Equation 2)
 *
 * With g = 1/(1 << g_shift), this becomes: α = α - (α >> g_shift)
 */
static inline uint32_t dcqcn_alpha_decay(uint32_t alpha, uint32_t g_shift)
{
	return alpha - (alpha >> g_shift);
}

/*
 * Alpha update on CNP: α = (1 - g) * α + g  (paper Equation 1)
 *
 * With g = 1/(1 << g_shift) and α in FXP16:
 *   α = α - (α >> g_shift) + (1 << (16 - g_shift))
 */
static inline uint32_t dcqcn_alpha_update_cnp(uint32_t alpha, uint32_t g_shift)
{
	alpha = alpha - (alpha >> g_shift) + (1 << (16 - g_shift));
	if (alpha > (1 << 16))
		alpha = (1 << 16);
	return alpha;
}

/*
 * Handle new flow initialization.
 *
 * Paper Section 3.1: "When a flow starts, it sends at full line rate"
 * Paper footnote 1: "Initial value of α is 1"
 */
static inline void dcqcn_handle_new_flow(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  cc_ctxt_dcqcn_t *ccctx,
					  doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t new_flow_rate = param[DCQCN_PARAM_NEW_FLOW_RATE];

	ccctx->cur_rate = new_flow_rate;
	ccctx->target_rate = new_flow_rate;
	ccctx->alpha = DCQCN_INITIAL_ALPHA;
	ccctx->last_alpha_ts = timestamp;
	ccctx->last_timer_ts = timestamp;
	ccctx->bytes_accumulated = 0;
	ccctx->t_count = 0;
	ccctx->bc_count = 0;
	ccctx->last_sent_32bytes = 0;

	results->rate = new_flow_rate;
	results->rtt_req = 0;

	DCQCN_TRACE(DCQCN_TRACE_FMT_NEW_FLOW, new_flow_rate, DCQCN_INITIAL_ALPHA, timestamp, 0, 0);
	DCQCN_TRACE_FLUSH();
}

/*
 * Handle CNP (Congestion Notification Packet) event.
 *
 * Implements the CutRate() function from Figure 7 of the paper:
 *   1. RT = RC                          (save current rate as target)
 *   2. RC = RC * (1 - α/2)             (Equation 1: reduce current rate)
 *   3. α = (1-g)*α + g                 (Equation 1: update alpha EWMA)
 *   4. Reset(Timer, ByteCounter, T, BC, AlphaTimer)
 *
 * Before cutting rate, we apply any pending alpha decays (Equation 2)
 * since the alpha timer may have fired while waiting for this CNP.
 */
static inline void dcqcn_handle_cnp(doca_pcc_dev_event_t *event,
				     uint32_t *param,
				     cc_ctxt_dcqcn_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t cur_rate = ccctx->cur_rate;
#ifdef DCQCN_DEBUG
	uint32_t old_rate = cur_rate;
#endif
	uint32_t alpha = ccctx->alpha;
	uint32_t g_shift = param[DCQCN_PARAM_G_SHIFT];
	uint32_t timer_period = param[DCQCN_PARAM_TIMER_PERIOD];

	/* Apply pending alpha decays (Equation 2) before CNP processing */
	uint32_t alpha_elapsed = dcqcn_diff_with_wrap32(timestamp, ccctx->last_alpha_ts);

	if (timer_period > 0 && alpha_elapsed >= timer_period) {
		uint32_t n_alpha = alpha_elapsed / timer_period;

		if (n_alpha > DCQCN_MAX_ALPHA_CATCHUP) {
			alpha = 0;
		} else {
			for (uint32_t i = 0; i < n_alpha; i++)
				alpha = dcqcn_alpha_decay(alpha, g_shift);
		}
	}

	/* Step 1 (Equation 1): RT = RC */
	ccctx->target_rate = cur_rate;

	/* Step 2 (Equation 1): RC = RC * (1 - α/2)
	 * α is in FXP16, so α/2 in FXP16 = α >> 1.
	 * (1 - α/2) in FXP16 = (1<<16) - (α >> 1).
	 * doca_pcc_dev_fxp_mult(FXP16_factor, FXP20_rate) → FXP20_result. */
	uint32_t half_alpha = alpha >> 1;
	uint32_t dec_factor = (1 << 16) - half_alpha;

	cur_rate = doca_pcc_dev_fxp_mult(dec_factor, cur_rate);

	/* Step 3 (Equation 1): α = (1-g)*α + g */
	alpha = dcqcn_alpha_update_cnp(alpha, g_shift);

	/* Step 4 (Figure 7): Reset(Timer, ByteCounter, T, BC, AlphaTimer) */
	ccctx->t_count = 0;
	ccctx->bc_count = 0;
	ccctx->last_timer_ts = timestamp;
	ccctx->last_alpha_ts = timestamp;
	ccctx->bytes_accumulated = 0;

	if (cur_rate < param[DCQCN_PARAM_MIN_RATE])
		cur_rate = param[DCQCN_PARAM_MIN_RATE];

	ccctx->cur_rate = cur_rate;
	ccctx->alpha = alpha;
	results->rate = cur_rate;
	results->rtt_req = 0;

	DCQCN_TRACE(DCQCN_TRACE_FMT_CNP, old_rate, cur_rate, alpha, dec_factor, timestamp);
	DCQCN_TRACE_FLUSH();
}

/*
 * Handle TX event: implements the rate increase logic from Figure 7.
 *
 * On each TX event we check three software timers/counters:
 *   1. Alpha timer (Equation 2): decay α every timer_period ns
 *   2. Rate increase timer (Figure 7): increment T, increase rate every timer_period ns
 *   3. Byte counter (Figure 7): increment BC, increase rate every B bytes sent
 *
 * The paper assumes NIC hardware timers that fire asynchronously; in DOCA's
 * event-driven model we advance them on TX events instead — equivalent for active
 * flows, with bounded catch-up covering idle gaps.
 */
static inline void dcqcn_handle_tx(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    cc_ctxt_dcqcn_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t cur_rate = ccctx->cur_rate;
	uint32_t alpha = ccctx->alpha;
	uint32_t g_shift = param[DCQCN_PARAM_G_SHIFT];
	uint32_t timer_period = param[DCQCN_PARAM_TIMER_PERIOD];
	uint32_t byte_threshold = param[DCQCN_PARAM_BYTE_COUNTER_TH];

	/* --- Alpha Timer (Equation 2): α = (1 - g) * α --- */
	uint32_t alpha_elapsed = dcqcn_diff_with_wrap32(timestamp, ccctx->last_alpha_ts);

	if (timer_period > 0 && alpha_elapsed >= timer_period) {
		uint32_t n_alpha = alpha_elapsed / timer_period;

		if (n_alpha > DCQCN_MAX_ALPHA_CATCHUP) {
			alpha = 0;
			ccctx->last_alpha_ts = timestamp;
		} else {
			for (uint32_t i = 0; i < n_alpha; i++)
				alpha = dcqcn_alpha_decay(alpha, g_shift);
			ccctx->last_alpha_ts += n_alpha * timer_period;
		}
		ccctx->alpha = alpha;
	}

	/* --- Rate Increase Timer (Figure 7: Timer expires → T++; rate increase) --- */
	uint32_t timer_elapsed = dcqcn_diff_with_wrap32(timestamp, ccctx->last_timer_ts);

	if (timer_period > 0 && timer_elapsed >= timer_period) {
		uint32_t n_timer = timer_elapsed / timer_period;

		/* Cap catch-up so a long idle gap can't compound into a rate jump. */
		if (n_timer > DCQCN_MAX_TIMER_CATCHUP)
			n_timer = DCQCN_MAX_TIMER_CATCHUP;

		for (uint32_t i = 0; i < n_timer; i++) {
			ccctx->t_count++;
			cur_rate = dcqcn_rate_increase(ccctx, cur_rate, param);
		}
		ccctx->last_timer_ts = timestamp;
	}

	/* --- Byte Counter (Figure 7: ByteCounter expires → BC++; rate increase) ---
	 * B is in bytes; the TX event exposes a cumulative sent_32bytes counter
	 * (32-byte units, 16-bit wrapping), so accumulate the wrapped delta * 32. */
	doca_pcc_dev_roce_tx_cntrs_t tx_cntrs = doca_pcc_dev_get_roce_tx_cntrs(event);
	uint16_t cur_sent_32bytes = tx_cntrs.sent_32bytes;
	uint16_t delta_32bytes = cur_sent_32bytes - ccctx->last_sent_32bytes; /* 16-bit wrap is fine */

	ccctx->last_sent_32bytes = cur_sent_32bytes;
	ccctx->bytes_accumulated += (uint32_t)delta_32bytes * 32;

	if (byte_threshold > 0 && ccctx->bytes_accumulated >= byte_threshold) {
		ccctx->bc_count++;
		ccctx->bytes_accumulated -= byte_threshold;
		cur_rate = dcqcn_rate_increase(ccctx, cur_rate, param);
	}

	if (cur_rate > DOCA_PCC_DEV_MAX_RATE)
		cur_rate = DOCA_PCC_DEV_MAX_RATE;
	if (cur_rate < param[DCQCN_PARAM_MIN_RATE])
		cur_rate = param[DCQCN_PARAM_MIN_RATE];

	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 0;

	DCQCN_TRACE(DCQCN_TRACE_FMT_TX, cur_rate, alpha, ccctx->t_count, ccctx->bc_count, ccctx->bytes_accumulated);
}

/*
 * Handle NACK event.
 *
 * The paper does not define NACK handling (it assumes a PFC fabric where loss is
 * rare). We apply a conservative 50% rate cut and reset the recovery state.
 */
static inline void dcqcn_handle_nack(doca_pcc_dev_event_t *event,
				      uint32_t *param,
				      cc_ctxt_dcqcn_t *ccctx,
				      doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t cur_rate = ccctx->cur_rate;

	cur_rate = cur_rate >> 1;

	if (cur_rate < param[DCQCN_PARAM_MIN_RATE])
		cur_rate = param[DCQCN_PARAM_MIN_RATE];

	/* Reset recovery state so the flow re-enters fast recovery */
	ccctx->target_rate = cur_rate;
	ccctx->t_count = 0;
	ccctx->bc_count = 0;
	ccctx->last_timer_ts = timestamp;
	ccctx->bytes_accumulated = 0;

	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 0;
}

void dcqcn_algo(doca_pcc_dev_event_t *event,
		uint32_t *param,
		uint32_t *counter,
		doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		doca_pcc_dev_results_t *results)
{
	cc_ctxt_dcqcn_t *dcqcn_ctx = (cc_ctxt_dcqcn_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = dcqcn_ctx->cur_rate;

	if (unlikely(cur_rate == 0)) {
		dcqcn_handle_new_flow(event, param, dcqcn_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		dcqcn_handle_tx(event, param, dcqcn_ctx, results);
		if (counter != NULL)
			counter[DCQCN_COUNTER_TX_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		dcqcn_handle_cnp(event, param, dcqcn_ctx, results);
		if (counter != NULL)
			counter[DCQCN_COUNTER_CNP_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		dcqcn_handle_nack(event, param, dcqcn_ctx, results);
		if (counter != NULL)
			counter[DCQCN_COUNTER_NACK_EVENT]++;
	} else {
		/* RTT events or unknown: pass through with current rate */
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t dcqcn_set_algo_params(uint32_t param_id_base,
					    uint32_t param_num,
					    const uint32_t *new_param_values,
					    uint32_t *params)
{
	/* Bound base and num individually AND together, else the copy below writes OOB. */
	if ((param_id_base >= DCQCN_PARAM_NUM) ||
	    (param_num > DCQCN_PARAM_NUM) ||
	    (param_id_base + param_num > DCQCN_PARAM_NUM))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* Copy the validated values into the live array -- the framework does not
	 * auto-copy (contract: OK means the callback set them). Without this, PPCC writes
	 * read back via cmd_type=0xA yet the algo keeps its init-time defaults. */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
