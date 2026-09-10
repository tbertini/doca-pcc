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
 * SMaRTT: sender-based ECN and delay windowed congestion control, the reaction-point
 * algorithm of Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed
 * & Timed Transport with Recycled Entropies", arXiv:2404.01630, 2024. Implements
 * Algorithm 1 (four-case loop, Section 3.2), Algorithm 2 (QuickAdapt, Section 3.3),
 * Algorithm 3 (FastIncrease, Section 3.4) and Wait-to-Decrease (Section 3.6.1).
 *
 * The delay signal is the fabric RTT measured on separate RTT probe events, because
 * BlueField-3 ACKs carry no timestamps; the ECN signal is a CNP-hold predicate, because
 * the hardware delivers rate-limited CNPs rather than per-packet marks. cwnd (FXP16
 * packets) is the control variable and is actuated through the FXP20 rate limiter via the
 * BDP identity. REPS (Section 3.6, per-packet load balancing) is not implemented.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "smartt_reps_ctxt.h"
#include "smartt_reps_params.h"
#include "smartt_reps_algo_params.h"
#include "smartt_reps.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Per-event trace emission and flush, both gated by SMARTT_REPS_TRACE_ENABLED(param). */
#define SMARTT_REPS_TRACE(param, fmt_id, a1, a2, a3, a4, a5) \
	do { \
		if (SMARTT_REPS_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_5(fmt_id, (uint64_t)(a1), (uint64_t)(a2), \
					     (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5)); \
	} while (0)
#define SMARTT_REPS_TRACE_FLUSH(param) \
	do { \
		if (SMARTT_REPS_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_flush(); \
	} while (0)

/* Abort time for stuck RTT requests. */
#define SMARTT_REPS_ABORT_TIME_NS (300000)

/*
 * Calculate wrap around in case of 32-bit timestamp overflow.
 * Local static copy: the ALWAYS_INLINE diff_with_wrap32 in pcc_common_dev.h does not
 * survive -flto when pulled into two translation units.
 */
static inline uint32_t smartt_reps_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* Saturating 16-bit add for the uint16 per-flow accumulators (acked/ignored/fast_inc). */
static inline uint16_t smartt_reps_sat_add16(uint16_t a, uint32_t b)
{
	uint32_t sum = (uint32_t)a + b;
	return (sum > 0xFFFFu) ? 0xFFFFu : (uint16_t)sum;
}

/* PPCC contract description strings, read out-of-band by the host and never dereferenced
 * on device. They are const volatile so LTO does not eliminate them as dead data. */
const volatile char smartt_reps_desc[] = "SMaRTT v1.0 (Bonato et al. arXiv:2404.01630; REPS omitted)";
static const volatile char smartt_reps_p_base_rtt_desc[]     = "BASE_RTT_NS, no-load fabric RTT brtt (ns)";
static const volatile char smartt_reps_p_target_rtt_desc[]   = "TARGET_RTT_NS, congestion threshold trtt (ns)";
static const volatile char smartt_reps_p_fd_desc[]           = "FD, Fair Decrease gain (FXP16)";
static const volatile char smartt_reps_p_md_desc[]           = "MD, Multiplicative Decrease gain (FXP16)";
static const volatile char smartt_reps_p_fi_desc[]           = "FI, Fair Increase gain (FXP16); gamma-scaled on device";
static const volatile char smartt_reps_p_mi_desc[]           = "MI, Multiplicative Increase gain (FXP16); 0=>derive; gamma-scaled on device";
static const volatile char smartt_reps_p_qa_scaling_desc[]   = "QA_SCALING, QuickAdapt target scale (FXP16)";
static const volatile char smartt_reps_p_fast_inc_k_desc[]   = "FAST_INC_K, MTUs added per coalesced ACK (FXP16 pkts)";
static const volatile char smartt_reps_p_fast_inc_thresh_desc[] = "FAST_INC_THRESH, rtt<=brtt*thresh tolerance (FXP16)";
static const volatile char smartt_reps_p_wtd_alpha_desc[]    = "WTD_ALPHA, avg_wtd EWMA weight (FXP16)";
static const volatile char smartt_reps_p_wtd_thresh_desc[]   = "WTD_THRESH, avg_wtd decrease gate (FXP16); 0 disables";
static const volatile char smartt_reps_p_ecn_hold_desc[]     = "ECN_CNP_HOLD_NS, CNP->ECN assertion window (ns)";
static const volatile char smartt_reps_p_max_cwnd_frac_desc[] = "MAX_CWND_FRAC, cwnd cap as fraction of BDP (FXP16)";
static const volatile char smartt_reps_p_min_cwnd_desc[]     = "MIN_CWND, cwnd floor (FXP16 pkts)";
static const volatile char smartt_reps_p_init_cwnd_desc[]    = "INIT_CWND, new-flow window (FXP16 pkts); 0=>BDP";
static const volatile char smartt_reps_p_mss_desc[]          = "MSS_BYTES, segment size for BDP (bytes)";
static const volatile char smartt_reps_p_line_rate_desc[]    = "LINE_RATE_MBPS, NIC line rate for BDP (Mbps)";
static const volatile char smartt_reps_p_min_rate_desc[]     = "MIN_RATE, rate floor (FXP20 of line rate)";
static const volatile char smartt_reps_p_trace_on_desc[]     = "TRACE_ON, runtime trace toggle (0/1); on if -DSMARTT_REPS_DEBUG";
static const volatile char smartt_reps_c_events_desc[]       = "EVENTS, total events dispatched";
static const volatile char smartt_reps_c_ack_desc[]          = "ACK_EVENTS, ACK events (control-loop driver)";
static const volatile char smartt_reps_c_rtt_desc[]          = "RTT_EVENTS, RTT probe responses";
static const volatile char smartt_reps_c_cnp_desc[]          = "CNP_EVENTS, CNP events (ECN-marking answer)";
static const volatile char smartt_reps_c_nack_desc[]         = "NACK_EVENTS, NACK events (loss signal)";
static const volatile char smartt_reps_c_fd_desc[]           = "FD_APPLIED, Fair Decrease applications (incl. FD in MD)";
static const volatile char smartt_reps_c_md_desc[]           = "MD_APPLIED, Multiplicative Decrease applications";
static const volatile char smartt_reps_c_fi_desc[]           = "FI_APPLIED, Fair Increase applications (incl. FI in MI)";
static const volatile char smartt_reps_c_mi_desc[]           = "MI_APPLIED, Multiplicative Increase applications";
static const volatile char smartt_reps_c_qa_desc[]           = "QA_FIRED, QuickAdapt window resets";
static const volatile char smartt_reps_c_fast_inc_desc[]     = "FAST_INC_ACKS, ACK events in active FastIncrease";
static const volatile char smartt_reps_c_wtd_desc[]          = "WTD_BLOCKED, ECN decrease wanted but avg_wtd < thresh";
static const volatile char smartt_reps_c_ignored_desc[]      = "ACKS_IGNORED, ACK events swallowed by post-QA blackout";
static const volatile char smartt_reps_c_clamp_min_desc[]    = "RATE_CLAMPED_MIN, rate clamped up to min floor";
static const volatile char smartt_reps_c_clamp_max_desc[]    = "RATE_CLAMPED_MAX, rate clamped down to MAX_RATE";
static const volatile char smartt_reps_c_psn_desc[]          = "PSN_ANOMALY, ΔPSN sanity-cap fallbacks to num_coalesced";

void smartt_reps_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = SMARTT_REPS_ALGO_ID;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(smartt_reps_desc);
	algo_def.algo_desc_addr = (uint64_t)smartt_reps_desc;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, SMARTT_REPS_NUM_PARAMS, SMARTT_REPS_NUM_COUNTERS);

	/* Register every parameter from the contract table with permissions=1 (updatable).
	 * The table's own min is passed, so parameters whose min is 0 accept a write of 0. */
#define SMARTT_REPS_REG_PARAM(idx, desc) \
	doca_pcc_dev_algo_init_param(algo_idx, (idx), smartt_reps_params[(idx)].def, smartt_reps_params[(idx)].max, \
				     smartt_reps_params[(idx)].min, 1, sizeof(desc), (uint64_t)(desc))

	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_BASE_RTT_NS,         smartt_reps_p_base_rtt_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_TARGET_RTT_NS,       smartt_reps_p_target_rtt_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_FD_FXP16,            smartt_reps_p_fd_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MD_FXP16,            smartt_reps_p_md_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_FI_FXP16,            smartt_reps_p_fi_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MI_FXP16,            smartt_reps_p_mi_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_QA_SCALING_FXP16,    smartt_reps_p_qa_scaling_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_FAST_INC_K_FXP16,    smartt_reps_p_fast_inc_k_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_FAST_INC_THRESH_FXP16, smartt_reps_p_fast_inc_thresh_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_WTD_ALPHA_FXP16,     smartt_reps_p_wtd_alpha_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_WTD_THRESH_FXP16,    smartt_reps_p_wtd_thresh_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_ECN_CNP_HOLD_NS,     smartt_reps_p_ecn_hold_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MAX_CWND_FRAC_FXP16, smartt_reps_p_max_cwnd_frac_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MIN_CWND_FXP16,      smartt_reps_p_min_cwnd_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_INIT_CWND_FXP16,     smartt_reps_p_init_cwnd_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MSS_BYTES,           smartt_reps_p_mss_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_LINE_RATE_MBPS,      smartt_reps_p_line_rate_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_MIN_RATE_FXP20,      smartt_reps_p_min_rate_desc);
	SMARTT_REPS_REG_PARAM(SMARTT_REPS_P_TRACE_ON,           smartt_reps_p_trace_on_desc);
#undef SMARTT_REPS_REG_PARAM

#define SMARTT_REPS_REG_COUNTER(idx, desc) \
	doca_pcc_dev_algo_init_counter(algo_idx, (idx), UINT32_MAX, 2, sizeof(desc), (uint64_t)(desc))

	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_EVENTS,           smartt_reps_c_events_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_ACK_EVENTS,       smartt_reps_c_ack_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_RTT_EVENTS,       smartt_reps_c_rtt_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_CNP_EVENTS,       smartt_reps_c_cnp_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_NACK_EVENTS,      smartt_reps_c_nack_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_FD,               smartt_reps_c_fd_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_MD,               smartt_reps_c_md_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_FI,               smartt_reps_c_fi_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_MI,               smartt_reps_c_mi_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_QA_FIRED,         smartt_reps_c_qa_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_FAST_INC,         smartt_reps_c_fast_inc_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_WTD_BLOCKED,      smartt_reps_c_wtd_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_ACKS_IGNORED,     smartt_reps_c_ignored_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_RATE_CLAMPED_MIN, smartt_reps_c_clamp_min_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_RATE_CLAMPED_MAX, smartt_reps_c_clamp_max_desc);
	SMARTT_REPS_REG_COUNTER(SMARTT_REPS_C_PSN_ANOMALY,      smartt_reps_c_psn_desc);
#undef SMARTT_REPS_REG_COUNTER
}

/* ===================== BDP / rate helpers ===================== */

/*
 * Compute the bandwidth-delay product at the given RTT:
 *   bdp_fxp16 = line_rate_mbps * rtt_ns * 2^16 / (mss_bytes * 8000)
 * Callers pass brtt for the paper's BDP (equation denominators, cwnd cap, gamma) and the
 * pacing RTT for actuation.
 *
 * @param [in]: PPCC parameter array
 * @rtt_ns [in]: RTT the BDP is evaluated at (ns)
 * @return: BDP in FXP16 packets, or 0 if the parameters make the denominator zero
 */
static inline uint32_t smartt_reps_compute_bdp_fxp16(uint32_t *param, uint32_t rtt_ns)
{
	uint64_t num = (uint64_t)param[SMARTT_REPS_P_LINE_RATE_MBPS] * rtt_ns * (1ull << 16);
	uint64_t den = (uint64_t)param[SMARTT_REPS_P_MSS_BYTES] * 8000ull;
	if (unlikely(den == 0))
		return 0;
	return (uint32_t)(num / den);
}

/*
 * Resolve the pacing denominator: the last stored fabric RTT, clamped to
 * [brtt, SMARTT_REPS_PACING_RTT_MAX_NS]. Dividing by the live RTT keeps inflight equal to
 * rate*rtt = cwnd, the defining property of a window.
 *
 * @param [in]: PPCC parameter array
 * @ctx [in]: per-flow context holding the last measured fabric RTT
 * @return: pacing RTT (ns)
 */
static inline uint32_t smartt_reps_pacing_rtt_ns(uint32_t *param, const cc_ctxt_smartt_reps_t *ctx)
{
	uint32_t brtt = param[SMARTT_REPS_P_BASE_RTT_NS];
	uint32_t rtt = ctx->last_rtt_ns;

	if (rtt <= brtt)
		return brtt;
	if (unlikely(rtt > SMARTT_REPS_PACING_RTT_MAX_NS))
		return SMARTT_REPS_PACING_RTT_MAX_NS;
	return rtt;
}

/*
 * Clamp the actuated rate to [min_rate, DOCA_PCC_DEV_MAX_RATE], reporting the two bounds
 * separately so the caller can drive the two clamp counters distinctly.
 *
 * @rate [in]: candidate rate (FXP20)
 * @min_rate [in]: minimum allowed rate (FXP20)
 * @clamp_min_flag [out]: set to 1 if rate was below min_rate (may be NULL)
 * @clamp_max_flag [out]: set to 1 if rate exceeded DOCA_PCC_DEV_MAX_RATE (may be NULL)
 * @return: clamped rate (FXP20)
 */
static inline uint32_t smartt_reps_clamp_rate(uint32_t rate, uint32_t min_rate,
					      uint32_t *clamp_min_flag, uint32_t *clamp_max_flag)
{
	uint32_t min_hit = 0, max_hit = 0;
	if (rate > DOCA_PCC_DEV_MAX_RATE) {
		rate = DOCA_PCC_DEV_MAX_RATE;
		max_hit = 1;
	}
	if (rate < min_rate) {
		rate = min_rate;
		min_hit = 1;
	}
	if (clamp_min_flag != NULL)
		*clamp_min_flag = min_hit;
	if (clamp_max_flag != NULL)
		*clamp_max_flag = max_hit;
	return rate;
}

/*
 * Convert cwnd to the rate that actuates it, via the BDP identity at the given pacing RTT:
 *   rate_fxp20 = clamp((cwnd_fxp16 << 20) / bdp_fxp16(rtt_ns), min_rate, MAX_RATE)
 * Bumps the two clamp counters and reports whether either bound hit, so the caller can OR
 * the CLAMPED bit into the trace branch code.
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @cwnd_fxp16 [in]: congestion window (FXP16 packets)
 * @rtt_ns [in]: pacing RTT the BDP is evaluated at (ns)
 * @clamped_flag [out]: set to 1 if either bound was hit (may be NULL)
 * @return: actuated rate (FXP20)
 */
static inline uint32_t smartt_reps_cwnd_to_rate(uint32_t *param, uint32_t *counter,
						uint32_t cwnd_fxp16, uint32_t rtt_ns,
						uint32_t *clamped_flag)
{
	uint32_t bdp_fxp16 = smartt_reps_compute_bdp_fxp16(param, rtt_ns);
	uint32_t rate;
	uint32_t clamp_min = 0, clamp_max = 0;

	if (unlikely(bdp_fxp16 == 0))
		rate = DOCA_PCC_DEV_MAX_RATE; /* fail open to line rate */
	else
		rate = (uint32_t)(((uint64_t)cwnd_fxp16 << 20) / bdp_fxp16);

	rate = smartt_reps_clamp_rate(rate, param[SMARTT_REPS_P_MIN_RATE_FXP20], &clamp_min, &clamp_max);
	if (counter != NULL) {
		if (clamp_min)
			counter[SMARTT_REPS_C_RATE_CLAMPED_MIN]++;
		if (clamp_max)
			counter[SMARTT_REPS_C_RATE_CLAMPED_MAX]++;
	}
	if (clamped_flag != NULL)
		*clamped_flag = (clamp_min || clamp_max);
	return rate;
}

/*
 * Clamp cwnd to [min_cwnd, max_cwnd_frac * BDP] (Algorithm 1 line 36, paper Section 3.5).
 * If the computed cap is below min_cwnd, min_cwnd wins, so the window is never clamped
 * below its own floor.
 *
 * @param [in]: PPCC parameter array
 * @cwnd_fxp16 [in]: candidate congestion window (FXP16 packets)
 * @return: clamped congestion window (FXP16 packets)
 */
static inline uint32_t smartt_reps_clamp_cwnd(uint32_t *param, uint32_t cwnd_fxp16)
{
	uint32_t min_cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	/* Cap at the base-RTT BDP: the cap must not inflate as congestion raises the RTT. */
	uint32_t bdp_fxp16 = smartt_reps_compute_bdp_fxp16(param, param[SMARTT_REPS_P_BASE_RTT_NS]);
	uint32_t cap = doca_pcc_dev_fxp_mult(param[SMARTT_REPS_P_MAX_CWND_FRAC_FXP16], bdp_fxp16);

	if (cap < min_cwnd)
		cap = min_cwnd;
	if (cwnd_fxp16 < min_cwnd)
		cwnd_fxp16 = min_cwnd;
	if (cwnd_fxp16 > cap)
		cwnd_fxp16 = cap;
	return cwnd_fxp16;
}

/*
 * Compute gamma = bdp/reference_bdp (paper Section 3.5 "Scaling"), the factor the fi and
 * mi gains are multiplied by so window growth stays proportional to the BDP of this
 * fabric. fd and md are not scaled. bdp_bytes = line_rate_mbps * base_rtt_ns / 8000.
 *
 * @param [in]: PPCC parameter array
 * @return: gamma in FXP16, saturated at UINT32_MAX
 */
static inline uint32_t smartt_reps_gamma_fxp16(uint32_t *param)
{
	uint64_t bdp_bytes = (uint64_t)param[SMARTT_REPS_P_LINE_RATE_MBPS] *
			     param[SMARTT_REPS_P_BASE_RTT_NS] / 8000ull;
	uint64_t g = (bdp_bytes << 16) / SMARTT_REPS_REF_BDP_BYTES;
	return (g > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)g;
}

/* Resolve the effective fi gain: the parameter scaled by gamma (paper Section 3.5). */
static inline uint32_t smartt_reps_effective_fi(uint32_t *param)
{
	return doca_pcc_dev_fxp_mult(param[SMARTT_REPS_P_FI_FXP16], smartt_reps_gamma_fxp16(param));
}

/* Resolve the effective mi gain: the parameter, or, on the mi == 0 sentinel,
 * brtt/(trtt-brtt) derived on device (paper Section 3.2.4), then scaled by gamma. */
static inline uint32_t smartt_reps_effective_mi(uint32_t *param)
{
	uint32_t mi = param[SMARTT_REPS_P_MI_FXP16];
	uint32_t brtt = param[SMARTT_REPS_P_BASE_RTT_NS];
	uint32_t trtt = param[SMARTT_REPS_P_TARGET_RTT_NS];

	if (mi == 0) {
		if (trtt <= brtt)
			mi = 2 * SMARTT_REPS_FXP16_ONE; /* 131072 */
		else
			mi = (uint32_t)(((uint64_t)brtt << 16) / (trtt - brtt));
	}
	return doca_pcc_dev_fxp_mult(mi, smartt_reps_gamma_fxp16(param));
}

/* ===================== delta-PSN acked-packet tracker ===================== */

/*
 * Resolve how many packets were acked or delivered since the last ACK/NACK event, from
 * the delta between first_sn and the stored PSN, and update the PSN state. An implausible
 * delta (above max(2*cwnd_pkts + 2, 256)) bumps the psn_anomaly counter and falls back to
 * num_coalesced.
 *
 * @ctx [in/out]: per-flow context holding the PSN tracker state
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @psn [in]: first_sn of this ACK/NACK event
 * @num_coalesced [in]: coalesced-packet count reported by the event, used as fallback
 * @return: packet count acked since the previous event, bounded by the sanity cap
 */
static inline uint32_t smartt_reps_track_acked_pkts(cc_ctxt_smartt_reps_t *ctx, uint32_t *counter,
						    uint32_t psn, uint32_t num_coalesced)
{
	uint32_t delta;
	uint32_t cwnd_pkts = ctx->cwnd_fxp16 >> 16;
	uint32_t cap = 2u * cwnd_pkts + 2u;

	if (cap < 256u)
		cap = 256u;

	if (ctx->flags & SMARTT_REPS_FLAG_PSN_VALID) {
		delta = smartt_reps_diff_with_wrap32(psn, ctx->last_ack_psn);
		if (delta > cap) {
			delta = num_coalesced;
			if (counter != NULL)
				counter[SMARTT_REPS_C_PSN_ANOMALY]++;
		}
	} else {
		delta = num_coalesced;
	}

	/* The num_coalesced fallback is a raw 16-bit field, so bound it by the same sanity
	 * cap: it feeds every downstream acked-proportional term. */
	if (delta > cap)
		delta = cap;

	ctx->last_ack_psn = psn;
	ctx->flags |= SMARTT_REPS_FLAG_PSN_VALID;
	return delta;
}

/* ===================== QuickAdapt (Algorithm 2, paper Section 3.3) ===================== */

/*
 * Run QuickAdapt, called on every ACK and NACK event, firing at most once per trtt epoch.
 * On fire it sets cwnd = max(acked, 1 MTU) * qa_scaling, arms the blackout with
 * ignore_target = the pre-fire cwnd in packets (Algorithm 2 line 8) and clears the ignored
 * count. Crossing an epoch boundary always resets qa_epoch_start and acked (line 12).
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context holding the epoch and blackout state
 * @now [in]: event timestamp (ns)
 * @cwnd_io [in/out]: congestion window (FXP16 packets), rewritten if QuickAdapt fires
 * @src [in]: trace annotation, 1 = ACK event, 2 = NACK event
 * @tsflow [in]: packed timestamp and flow tag, used only as the trace arg1
 * @return: 1 if QuickAdapt fired and changed cwnd, 0 otherwise
 */
static inline uint32_t smartt_reps_quick_adapt(uint32_t *param, uint32_t *counter,
					       cc_ctxt_smartt_reps_t *ctx, uint32_t now,
					       uint32_t *cwnd_io, uint32_t src, uint64_t tsflow)
{
	uint32_t trtt = param[SMARTT_REPS_P_TARGET_RTT_NS];
	uint32_t adapted = 0;

	if (smartt_reps_diff_with_wrap32(now, ctx->qa_epoch_start) >= trtt) { /* now >= end */
		/* No fire before the first full epoch has been established. */
		if ((ctx->flags & SMARTT_REPS_FLAG_TRIGGER_QA) &&
		    (ctx->flags & SMARTT_REPS_FLAG_QA_EPOCH_VALID)) {
			uint32_t prev_cwnd = *cwnd_io;
			uint32_t ignore_target = prev_cwnd >> 16;    /* bytes_to_ignore ~= old cwnd, pkts */
			if (prev_cwnd & 0xFFFFu)                      /* round up */
				ignore_target += 1;
			if (ignore_target < 1)
				ignore_target = 1;
			ctx->packets_to_ignore = (ignore_target > 0xFFFFu) ? 0xFFFFu : (uint16_t)ignore_target;
			ctx->ignored_pkts = 0;

			uint32_t acked = ctx->acked_pkts;
			uint32_t qa_floor_pkts = (acked >= 1) ? acked : 1; /* max(acked, mtu) in packets */
			/* Explicit uint64 with saturation: doca_pcc_dev_fxp_mult truncates its
			 * uint32 result, and this product exceeds uint32 for parameter values
			 * that are within the registered bounds. */
			uint64_t cwnd64 = (((uint64_t)qa_floor_pkts << 16) *
					   param[SMARTT_REPS_P_QA_SCALING_FXP16]) >> 16;
			uint32_t cwnd = (cwnd64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd64;
			cwnd = smartt_reps_clamp_cwnd(param, cwnd); /* caller clamps again; idempotent */
			*cwnd_io = cwnd;

			ctx->flags &= ~SMARTT_REPS_FLAG_TRIGGER_QA;
			adapted = 1;
			if (counter != NULL)
				counter[SMARTT_REPS_C_QA_FIRED]++;

			SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_QA, tsflow, src,
					  ctx->packets_to_ignore, cwnd, ctx->cur_rate);
		}
		ctx->qa_epoch_start = now;
		ctx->flags |= SMARTT_REPS_FLAG_QA_EPOCH_VALID;
		ctx->acked_pkts = 0; /* epoch accumulator reset (Algorithm 2 line 12) */
	}

	return adapted;
}

/* ===================== FastIncrease (Algorithm 3, paper Section 3.4) ===================== */

/*
 * Run FastIncrease, which engages while rtt <= brtt * fast_inc_thresh and ECN is not
 * asserted. Once active, or once the accumulated packet counter exceeds cwnd, it adds
 * fast_inc_k MTUs per coalesced ACK arrival. When not engaged it clears the counter and
 * the fi_active flag.
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context holding the FastIncrease state machine
 * @rtt [in]: last measured fabric RTT (ns)
 * @ecn [in]: 1 if the ECN predicate is asserted for this event
 * @pkts_acked [in]: packets acked by this event
 * @num_coalesced [in]: coalesced-packet count reported by the event
 * @cwnd_io [in/out]: congestion window (FXP16 packets), rewritten if FastIncrease fires
 * @tsflow [in]: packed timestamp and flow tag, used only as the trace arg1
 * @return: 1 if FastIncrease fired and changed cwnd, 0 otherwise
 */
static inline uint32_t smartt_reps_fast_increase(uint32_t *param, uint32_t *counter,
						 cc_ctxt_smartt_reps_t *ctx, uint32_t rtt,
						 uint32_t ecn, uint32_t pkts_acked, uint32_t num_coalesced,
						 uint32_t *cwnd_io, uint64_t tsflow)
{
	uint32_t brtt = param[SMARTT_REPS_P_BASE_RTT_NS];
	uint32_t thresh = param[SMARTT_REPS_P_FAST_INC_THRESH_FXP16];
	uint32_t rtt_bound = doca_pcc_dev_fxp_mult(brtt, thresh); /* brtt * fast_inc_thresh */

	if ((rtt <= rtt_bound) && !ecn) {
		ctx->fast_inc_count = smartt_reps_sat_add16(ctx->fast_inc_count, pkts_acked);
		uint32_t count_fxp16 = (uint32_t)ctx->fast_inc_count << 16;
		if ((count_fxp16 > *cwnd_io) || (ctx->flags & SMARTT_REPS_FLAG_FI_ACTIVE)) {
			uint64_t inc = (uint64_t)param[SMARTT_REPS_P_FAST_INC_K_FXP16] * num_coalesced; /* k MTUs/ACK */
			uint64_t cwnd = (uint64_t)*cwnd_io + inc;
			*cwnd_io = (cwnd > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd;
			ctx->flags |= SMARTT_REPS_FLAG_FI_ACTIVE;
			if (counter != NULL)
				counter[SMARTT_REPS_C_FAST_INC]++;
			/* Widen to uint64_t before shifting: fast_inc_count is uint16_t, so
			 * shifting it left by 16 in promoted int arithmetic is undefined. */
			SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_FASTINC, tsflow,
					  (((uint64_t)(ctx->fast_inc_count & 0xFFFF) << 16) | (num_coalesced & 0xFFFF)),
					  rtt, *cwnd_io, ctx->cur_rate);
			return 1;
		}
		return 0; /* still accumulating: the four cases run this event */
	}

	ctx->fast_inc_count = 0;
	ctx->flags &= ~SMARTT_REPS_FLAG_FI_ACTIVE;
	return 0;
}

/* ============ four-case window updates (paper Section 3.2, FXP16 packets) ============ */

/*
 * Apply Fair Decrease, cwnd -= (cwnd/bdp) * fd * size (Eq. 1, paper Section 3.2.1). If
 * the decrement would reach or pass cwnd, the window collapses to min_cwnd instead.
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @cwnd_io [in/out]: congestion window (FXP16 packets)
 * @bdp [in]: bandwidth-delay product at the base RTT (FXP16 packets)
 * @pkts_acked [in]: packets acked by this event
 */
static inline void smartt_reps_apply_fd(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t bdp, uint32_t pkts_acked)
{
	uint32_t cwnd = *cwnd_io;
	/* (cwnd/bdp)*fd in FXP16, then * pkts_acked (integer packets). */
	uint32_t cwnd_over_bdp = (bdp != 0) ? (uint32_t)(((uint64_t)cwnd << 16) / bdp) : 0;
	uint32_t term_fxp16 = doca_pcc_dev_fxp_mult(cwnd_over_bdp, param[SMARTT_REPS_P_FD_FXP16]);
	uint64_t dec = (uint64_t)term_fxp16 * pkts_acked;

	if (dec >= cwnd)
		*cwnd_io = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	else
		*cwnd_io = cwnd - (uint32_t)dec;

	if (counter != NULL)
		counter[SMARTT_REPS_C_FD]++;
}

/*
 * Apply Multiplicative Decrease and then Fair Decrease (Eq. 2, paper Section 3.2.2):
 *   dec = ((rtt-trtt)/rtt) * md * size, capped at size MTU; cwnd -= dec; then FD.
 * Requires rtt > trtt > 0, which the caller's branch condition guarantees. If the
 * decrement would reach or pass cwnd, the window collapses to min_cwnd instead.
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @cwnd_io [in/out]: congestion window (FXP16 packets)
 * @rtt [in]: last measured fabric RTT (ns)
 * @trtt [in]: target RTT (ns)
 * @bdp [in]: bandwidth-delay product at the base RTT (FXP16 packets)
 * @pkts_acked [in]: packets acked by this event
 */
static inline void smartt_reps_apply_md(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t rtt, uint32_t trtt,
					uint32_t bdp, uint32_t pkts_acked)
{
	uint32_t cwnd = *cwnd_io;
	uint32_t ratio = (rtt != 0) ? (uint32_t)((((uint64_t)(rtt - trtt)) << 16) / rtt) : 0; /* (rtt-trtt)/rtt FXP16 */
	uint32_t md_term = doca_pcc_dev_fxp_mult(ratio, param[SMARTT_REPS_P_MD_FXP16]);        /* * md */
	uint64_t dec = (uint64_t)md_term * pkts_acked;                                         /* FXP16 pkts */
	uint64_t cap = (uint64_t)pkts_acked << 16;                                             /* at most pkts_acked */
	if (dec > cap)
		dec = cap;

	if (dec >= cwnd)
		*cwnd_io = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	else
		*cwnd_io = cwnd - (uint32_t)dec;

	if (counter != NULL)
		counter[SMARTT_REPS_C_MD]++;

	/* Paper Section 3.2.2: MD additionally applies FD. */
	smartt_reps_apply_fd(param, counter, cwnd_io, bdp, pkts_acked);
}

/*
 * Apply Fair Increase, cwnd += (size/cwnd) * mtu * fi with fi gamma-scaled (Eq. 3, paper
 * Section 3.2.3). mtu is 1.0 = 65536, so (size/cwnd)*mtu is (size<<32)/cwnd in FXP16.
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @cwnd_io [in/out]: congestion window (FXP16 packets)
 * @pkts_acked [in]: packets acked by this event
 */
static inline void smartt_reps_apply_fi(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t pkts_acked)
{
	uint32_t cwnd = *cwnd_io;
	/* (size/cwnd)*mtu in FXP16. The quotient can exceed uint32 when min_cwnd is
	 * configured below one MTU, so saturate rather than truncate. */
	uint64_t acked_over_cwnd_mtu64 = (cwnd != 0) ? (((uint64_t)pkts_acked << 32) / cwnd) : 0;
	uint32_t acked_over_cwnd_mtu = (acked_over_cwnd_mtu64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu :
				       (uint32_t)acked_over_cwnd_mtu64;
	uint32_t inc_fxp16 = doca_pcc_dev_fxp_mult(acked_over_cwnd_mtu, smartt_reps_effective_fi(param));
	uint64_t cwnd_new = (uint64_t)cwnd + inc_fxp16;
	*cwnd_io = (cwnd_new > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd_new;

	if (counter != NULL)
		counter[SMARTT_REPS_C_FI]++;
}

/*
 * Apply Multiplicative Increase and then Fair Increase (Eq. 4, paper Section 3.2.4):
 *   inc = min( ((trtt-rtt)/rtt) * (size/cwnd) * mtu * mi , size MTU ); cwnd += inc; then FI.
 * The caller's branch gives rtt <= trtt; rtt == 0 and rtt == trtt are handled here (ratio term 0).
 *
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @cwnd_io [in/out]: congestion window (FXP16 packets)
 * @rtt [in]: last measured fabric RTT (ns)
 * @trtt [in]: target RTT (ns)
 * @mi [in]: effective, gamma-scaled MI gain (FXP16)
 * @pkts_acked [in]: packets acked by this event
 */
static inline void smartt_reps_apply_mi(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t rtt, uint32_t trtt,
					uint32_t mi, uint32_t pkts_acked)
{
	uint32_t cwnd = *cwnd_io;
	uint32_t ratio = (rtt != 0) ? (uint32_t)((((uint64_t)(trtt - rtt)) << 16) / rtt) : 0; /* (trtt-rtt)/rtt FXP16 */
	/* (size/cwnd)*mtu in FXP16, saturated rather than truncated. */
	uint64_t acked_over_cwnd_mtu64 = (cwnd != 0) ? (((uint64_t)pkts_acked << 32) / cwnd) : 0;
	uint32_t acked_over_cwnd_mtu = (acked_over_cwnd_mtu64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu :
				       (uint32_t)acked_over_cwnd_mtu64;
	/* ratio * acked_over_cwnd_mtu * mi: chain two FXP16 multiplies. */
	uint32_t inc_fxp16 = doca_pcc_dev_fxp_mult(doca_pcc_dev_fxp_mult(ratio, acked_over_cwnd_mtu), mi);
	uint64_t inc = inc_fxp16;
	uint64_t cap = (uint64_t)pkts_acked << 16; /* at most pkts_acked */
	if (inc > cap)
		inc = cap;
	uint64_t cwnd_new = (uint64_t)cwnd + inc;
	*cwnd_io = (cwnd_new > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd_new;

	if (counter != NULL)
		counter[SMARTT_REPS_C_MI]++;

	/* Paper Section 3.2.4: MI is followed by FI. */
	smartt_reps_apply_fi(param, counter, cwnd_io, pkts_acked);
}

/* ===================== event handlers ===================== */

/*
 * Initialise a new flow, detected by the cur_rate == 0 sentinel: cwnd = init_cwnd, or the
 * BDP when init_cwnd is 0; rtt = brtt; all PSN, QuickAdapt and FastIncrease state cleared.
 * Actuates the initial rate and requests the first RTT probe.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [out]: per-flow context to initialise
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_new_flow(doca_pcc_dev_event_t *event,
					       uint32_t *param,
					       uint32_t *counter,
					       cc_ctxt_smartt_reps_t *ctx,
					       doca_pcc_dev_results_t *results)
{
	uint32_t now = doca_pcc_dev_get_timestamp(event);
	/* arg1 for every trace emitted below. */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t init_cwnd = param[SMARTT_REPS_P_INIT_CWND_FXP16];
	uint32_t bdp = smartt_reps_compute_bdp_fxp16(param, param[SMARTT_REPS_P_BASE_RTT_NS]);
	uint32_t cwnd = (init_cwnd != 0) ? init_cwnd : bdp;
	uint32_t clamped = 0;
	uint32_t rate;

	if (cwnd == 0) /* init_cwnd and bdp both 0: fall back to the cwnd floor */
		cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];

	/* No RTT sample yet: pace at brtt. */
	rate = smartt_reps_cwnd_to_rate(param, counter, cwnd, param[SMARTT_REPS_P_BASE_RTT_NS], &clamped);

	ctx->cur_rate = rate;
	ctx->cwnd_fxp16 = cwnd;
	ctx->last_rtt_ns = param[SMARTT_REPS_P_BASE_RTT_NS]; /* rtt = brtt until first probe */
	ctx->last_cnp_ts = 0;
	ctx->qa_epoch_start = now;
	ctx->last_ack_psn = 0;
	ctx->probe_start_ts = now;
	ctx->acked_pkts = 0;
	ctx->packets_to_ignore = 0;
	ctx->ignored_pkts = 0;
	ctx->avg_wtd_fxp16 = 0;
	ctx->fast_inc_count = 0;
	ctx->flags = 0;
	ctx->abort_cnt = 0;
	ctx->tx_since_probe_req = 1;
	ctx->reserved8 = 0;
	ctx->reserved16 = 0;
	ctx->reserved32 = 0;

	results->rate = rate;
	results->rtt_req = 1; /* request first RTT probe */

	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_ACK, tsflow,
			  (uint64_t)SMARTT_REPS_BRANCH_FI | (clamped ? SMARTT_REPS_BRANCH_CLAMPED_BIT : 0),
			  ctx->last_rtt_ns, cwnd, rate);
	SMARTT_REPS_TRACE_FLUSH(param);
}

/*
 * Handle an ACK event: the main control loop (Algorithm 1, paper Sections 3.2 to 3.6.1).
 * Consumes the stored fabric RTT, kept fresh by probe events, and the CNP-hold ECN
 * predicate; runs QuickAdapt, FastIncrease and the four-case loop; then clamps cwnd and
 * actuates the resulting rate.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_ack(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  uint32_t *counter,
					  cc_ctxt_smartt_reps_t *ctx,
					  doca_pcc_dev_results_t *results)
{
	if (counter != NULL)
		counter[SMARTT_REPS_C_ACK_EVENTS]++;

	uint32_t now = doca_pcc_dev_get_timestamp(event);
	/* arg1 for every trace emitted below, computed once and threaded into the QuickAdapt
	 * and FastIncrease helpers so all traces from this event carry the same tsflow. */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t psn = doca_pcc_dev_get_roce_ack_first_sn(event);
	doca_pcc_dev_ack_nack_cnp_extra_t extra = doca_pcc_dev_get_ack_nack_cnp_extra(event);
	uint32_t num_coalesced = extra.num_coalesced;
	if (num_coalesced == 0)
		num_coalesced = 1; /* treat 0 as 1 */

	uint32_t pkts_acked = smartt_reps_track_acked_pkts(ctx, counter, psn, num_coalesced);

	uint32_t rtt  = ctx->last_rtt_ns;
	uint32_t trtt = param[SMARTT_REPS_P_TARGET_RTT_NS];
	/* The paper's bdp (equation denominators) stays at the base RTT. */
	uint32_t bdp  = smartt_reps_compute_bdp_fxp16(param, param[SMARTT_REPS_P_BASE_RTT_NS]);

	/* p.ecn is asserted when a CNP arrived within the hold window. */
	uint32_t ecn = 0;
	if (ctx->flags & SMARTT_REPS_FLAG_CNP_SEEN) {
		uint32_t since_cnp = smartt_reps_diff_with_wrap32(now, ctx->last_cnp_ts);
		ecn = (since_cnp <= param[SMARTT_REPS_P_ECN_CNP_HOLD_NS]) ? 1 : 0;
	}

	/* Algorithm 1 lines 4-5: accumulate acked for the QuickAdapt epoch and ignored for
	 * the post-QuickAdapt blackout. */
	ctx->acked_pkts = smartt_reps_sat_add16(ctx->acked_pkts, pkts_acked);
	ctx->ignored_pkts = smartt_reps_sat_add16(ctx->ignored_pkts, pkts_acked);

	/* Raw ACK-signal trace: reports arrival and the resolved delta-PSN. */
	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_SIG, tsflow, psn,
			  ((uint64_t)((pkts_acked & 0xFFFF) << 16) | (num_coalesced & 0xFFFF)),
			  ctx->avg_wtd_fxp16, 0);

	/* Algorithm 1 lines 8-10: post-QuickAdapt blackout. While still ignoring the last
	 * window's in-flight bytes, swallow the ACK and leave the rate unchanged. */
	if (ctx->ignored_pkts < ctx->packets_to_ignore) {
		if (counter != NULL)
			counter[SMARTT_REPS_C_ACKS_IGNORED]++;
		SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_ACK, tsflow,
				  (uint64_t)SMARTT_REPS_BRANCH_BLACKOUT | (ecn ? SMARTT_REPS_BRANCH_ECN_BIT : 0),
				  rtt, ctx->cwnd_fxp16, ctx->cur_rate);
		SMARTT_REPS_TRACE_FLUSH(param);
		results->rate = ctx->cur_rate; /* rate unchanged */
		results->rtt_req = 0;
		return;
	}

	/* Wait-to-Decrease (paper Section 3.6.1): avg_wtd = alpha*(ecn?1:0) +
	 * (1-alpha)*avg_wtd, computed in uint32 FXP16 and capped for the uint16 field. */
	{
		uint32_t alpha = param[SMARTT_REPS_P_WTD_ALPHA_FXP16];
		uint32_t one_minus_alpha = (alpha <= SMARTT_REPS_FXP16_ONE) ? (SMARTT_REPS_FXP16_ONE - alpha) : 0;
		uint32_t ecn_term = ecn ? alpha : 0; /* alpha * 1 or alpha * 0, FXP16 */
		uint32_t decay = doca_pcc_dev_fxp_mult(one_minus_alpha, ctx->avg_wtd_fxp16);
		uint32_t avg = ecn_term + decay;
		ctx->avg_wtd_fxp16 = (avg > 0xFFFFu) ? 0xFFFFu : (uint16_t)avg;
	}
	uint32_t can_decrease = (ctx->avg_wtd_fxp16 >= param[SMARTT_REPS_P_WTD_THRESH_FXP16]);

	uint32_t cwnd = ctx->cwnd_fxp16;
	uint32_t branch;
	uint32_t trace_bits = (ecn ? SMARTT_REPS_BRANCH_ECN_BIT : 0) |
			      (can_decrease ? SMARTT_REPS_BRANCH_CAN_DEC_BIT : 0);

	/* Both QuickAdapt and FastIncrease are evaluated on every ACK, so the FastIncrease
	 * state machine stays consistent even when QuickAdapt fires. Both mutate cwnd through
	 * the pointer, so when both fire FastIncrease stacks on the QuickAdapt-reset cwnd. If
	 * either fires, the four cases are skipped and the event goes straight to clamp and
	 * actuate, and the trace branch code reports QuickAdapt in preference. */
	uint32_t qa_fired = smartt_reps_quick_adapt(param, counter, ctx, now, &cwnd, 1 /* src=ack */, tsflow);
	uint32_t fastinc_fired = smartt_reps_fast_increase(param, counter, ctx, rtt, ecn, pkts_acked,
							   num_coalesced, &cwnd, tsflow);

	if (qa_fired) {
		branch = SMARTT_REPS_BRANCH_QA_ADAPTED;
	} else if (fastinc_fired) {
		branch = SMARTT_REPS_BRANCH_FASTINC;
	} else {
		/* --- four-case control loop (paper Section 3.2) --- */
		if (ecn && (rtt <= trtt) && can_decrease) {
			smartt_reps_apply_fd(param, counter, &cwnd, bdp, pkts_acked);   /* Section 3.2.1 */
			branch = SMARTT_REPS_BRANCH_FD;
		} else if (ecn && (rtt > trtt) && can_decrease) {
			smartt_reps_apply_md(param, counter, &cwnd, rtt, trtt, bdp, pkts_acked); /* Section 3.2.2 */
			branch = SMARTT_REPS_BRANCH_MD;
		} else if (!ecn && (rtt > trtt)) {
			smartt_reps_apply_fi(param, counter, &cwnd, pkts_acked);        /* Section 3.2.3 */
			branch = SMARTT_REPS_BRANCH_FI;
		} else if (!ecn && (rtt <= trtt)) {
			uint32_t mi = smartt_reps_effective_mi(param);
			smartt_reps_apply_mi(param, counter, &cwnd, rtt, trtt, mi, pkts_acked); /* Section 3.2.4 */
			branch = SMARTT_REPS_BRANCH_MI;
		} else {
			/* ecn and not can_decrease: Wait-to-Decrease blocked the decrease. */
			branch = SMARTT_REPS_BRANCH_WTD_BLOCKED;
			if (counter != NULL)
				counter[SMARTT_REPS_C_WTD_BLOCKED]++;
		}
	}

	/* Algorithm 1 line 36: clamp cwnd to [min_cwnd, max_cwnd_frac * BDP]. */
	cwnd = smartt_reps_clamp_cwnd(param, cwnd);
	ctx->cwnd_fxp16 = cwnd;

	/* Actuate: cwnd -> FXP20 rate via the BDP identity at the live pacing RTT. */
	uint32_t clamped = 0;
	uint32_t rate = smartt_reps_cwnd_to_rate(param, counter, cwnd,
						 smartt_reps_pacing_rtt_ns(param, ctx), &clamped);
	if (clamped)
		branch |= SMARTT_REPS_BRANCH_CLAMPED_BIT;
	branch |= trace_bits;

	ctx->cur_rate = rate;
	results->rate = rate;
	results->rtt_req = 0;

	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_ACK, tsflow, branch, rtt, cwnd, rate);
	SMARTT_REPS_TRACE_FLUSH(param);
}

/*
 * Handle a NACK event, the trimming and timeout surrogate (Algorithm 1 lines 28-34). NACK
 * first_sn is the receiver's expected PSN, so its delta counts delivered packets and feeds
 * both the QuickAdapt epoch and the blackout accumulator. The window loses one MTU,
 * trigger_qa is set, and QuickAdapt runs once the blackout has been served.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_nack(doca_pcc_dev_event_t *event,
					   uint32_t *param,
					   uint32_t *counter,
					   cc_ctxt_smartt_reps_t *ctx,
					   doca_pcc_dev_results_t *results)
{
	if (counter != NULL)
		counter[SMARTT_REPS_C_NACK_EVENTS]++;

	uint32_t now = doca_pcc_dev_get_timestamp(event);
	/* arg1 for the trace below, also threaded into the QuickAdapt helper. */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t psn = doca_pcc_dev_get_roce_ack_first_sn(event);
	doca_pcc_dev_ack_nack_cnp_extra_t extra = doca_pcc_dev_get_ack_nack_cnp_extra(event);
	uint32_t num_coalesced = extra.num_coalesced;
	if (num_coalesced == 0)
		num_coalesced = 1;

	uint32_t pkts_acked = smartt_reps_track_acked_pkts(ctx, counter, psn, num_coalesced);
	ctx->acked_pkts = smartt_reps_sat_add16(ctx->acked_pkts, pkts_acked);
	ctx->ignored_pkts = smartt_reps_sat_add16(ctx->ignored_pkts, pkts_acked);

	/* Line 29: cwnd -= 1 MTU, the lost packet's p.size, floored at min_cwnd. */
	uint32_t cwnd = ctx->cwnd_fxp16;
	uint32_t min_cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	if (cwnd > SMARTT_REPS_MTU_FXP16 + min_cwnd)
		cwnd -= SMARTT_REPS_MTU_FXP16;
	else
		cwnd = min_cwnd;

	/* Line 30: trigger_qa = true. */
	ctx->flags |= SMARTT_REPS_FLAG_TRIGGER_QA;

	/* Lines 32-34: if ignored >= ignore_target, run QuickAdapt now. */
	if (ctx->ignored_pkts >= ctx->packets_to_ignore)
		(void)smartt_reps_quick_adapt(param, counter, ctx, now, &cwnd, 2 /* src=nack */, tsflow);

	cwnd = smartt_reps_clamp_cwnd(param, cwnd);
	ctx->cwnd_fxp16 = cwnd;

	uint32_t clamped = 0;
	uint32_t rate = smartt_reps_cwnd_to_rate(param, counter, cwnd,
						 smartt_reps_pacing_rtt_ns(param, ctx), &clamped);
	ctx->cur_rate = rate;
	results->rate = rate;
	results->rtt_req = 0;

	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_LOSS, tsflow, psn,
			  ((uint64_t)(ctx->ignored_pkts) << 1) |
			  (((ctx->flags & SMARTT_REPS_FLAG_TRIGGER_QA) ? 1 : 0) & 1),
			  cwnd, rate);
	SMARTT_REPS_TRACE_FLUSH(param);
}

/*
 * Handle a CNP event by recording the arrival timestamp for the ECN-hold predicate. The
 * decrease itself happens in the ACK loop via p.ecn, so the rate is unchanged here.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_cnp(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  uint32_t *counter,
					  cc_ctxt_smartt_reps_t *ctx,
					  doca_pcc_dev_results_t *results)
{
	if (counter != NULL)
		counter[SMARTT_REPS_C_CNP_EVENTS]++;

	ctx->last_cnp_ts = doca_pcc_dev_get_timestamp(event);
	ctx->flags |= SMARTT_REPS_FLAG_CNP_SEEN;

	results->rate = ctx->cur_rate;
	results->rtt_req = 0;
}

/*
 * Handle an RTT probe response: store the fresh fabric RTT for the ACK loop to consume,
 * clear the probe-abort state and re-arm the next probe. cwnd and the rate are unchanged,
 * since the window only moves on ACK and NACK events.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @counter [in/out]: PPCC counter array (may be NULL)
 * @ctx [in/out]: per-flow context
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_rtt(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  uint32_t *counter,
					  cc_ctxt_smartt_reps_t *ctx,
					  doca_pcc_dev_results_t *results)
{
	if (counter != NULL)
		counter[SMARTT_REPS_C_RTT_EVENTS]++;

	/* Drop RTT events that match no outstanding probe, that is, a response to a probe
	 * already aborted. */
	if (unlikely(!(ctx->flags & SMARTT_REPS_FLAG_RTT_PROBE_OUTSTANDING) &&
		     (ctx->tx_since_probe_req == 0))) {
		results->rate = ctx->cur_rate;
		results->rtt_req = 0;
		return;
	}
	ctx->flags &= ~SMARTT_REPS_FLAG_RTT_PROBE_OUTSTANDING;
	ctx->abort_cnt = 0;

	uint32_t req_send  = doca_pcc_dev_get_rtt_req_send_timestamp(event);  /* t1 */
	uint32_t req_recv  = doca_pcc_dev_get_rtt_req_recv_timestamp(event);  /* t2 */
	uint32_t resp_send = doca_pcc_dev_get_rtt_resp_send_timestamp(event); /* t4 */
	uint32_t arrival   = doca_pcc_dev_get_timestamp(event);              /* t6 */

	uint32_t total_rtt = smartt_reps_diff_with_wrap32(arrival, req_send);
	uint32_t np_turnaround = 0;
	/* resp_send and req_recv are populated by the notification point; when either is
	 * absent the fabric RTT falls back to the total RTT. */
	if (likely(resp_send != 0 && req_recv != 0))
		np_turnaround = smartt_reps_diff_with_wrap32(resp_send, req_recv);
	uint32_t fabric_rtt = (np_turnaround < total_rtt) ? (total_rtt - np_turnaround) : total_rtt;

	ctx->last_rtt_ns = fabric_rtt;

	results->rate = ctx->cur_rate;      /* rate unchanged on probe events */
	results->rtt_req = 1;               /* re-arm next RTT probe */
	ctx->tx_since_probe_req = 1;
}

/*
 * Handle a TX event: the RTT-probe abort and retry state machine. Without it a single
 * dropped RTT response stalls the flow forever. cwnd is unchanged.
 *
 * @event [in]: event being handled
 * @param [in]: PPCC parameter array
 * @ctx [in/out]: per-flow context holding the probe state machine
 * @results [out]: result struct carrying the rate and rtt_req back to hardware
 */
static inline void smartt_reps_handle_tx(doca_pcc_dev_event_t *event,
					 uint32_t *param,
					 cc_ctxt_smartt_reps_t *ctx,
					 doca_pcc_dev_results_t *results)
{
	uint8_t rtt_req = 0;
	uint32_t probe_in_flight = (ctx->flags & SMARTT_REPS_FLAG_RTT_PROBE_OUTSTANDING) ? 1 : 0;
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

	if (unlikely((ev_attr.flags & DOCA_PCC_DEV_TX_FLAG_RTT_REQ_SENT) && (probe_in_flight == 0))) {
		ctx->flags |= SMARTT_REPS_FLAG_RTT_PROBE_OUTSTANDING;
		ctx->tx_since_probe_req = 0;
		ctx->probe_start_ts = timestamp;
	} else {
		uint32_t probe_out_time_ns = timestamp - ctx->probe_start_ts;

		if (unlikely(ctx->probe_start_ts > timestamp))
			probe_out_time_ns += UINT32_MAX;

		if (probe_in_flight == 0) {
			probe_out_time_ns = 0;
			ctx->tx_since_probe_req += 1;
		}
		/* The stored abort_cnt is capped at use as well as at growth: a shift by
		 * 32 or more is undefined behaviour. */
		uint32_t abort_threshold = (uint32_t)SMARTT_REPS_ABORT_TIME_NS
					   << ((ctx->abort_cnt < 12) ? ctx->abort_cnt : 12);
		if (unlikely((probe_out_time_ns > abort_threshold) ||
			     (ctx->tx_since_probe_req > 2))) {
			rtt_req = 1;
			if ((probe_out_time_ns > abort_threshold) && (ctx->abort_cnt < 12))
				ctx->abort_cnt += 1;
			ctx->tx_since_probe_req = 1;
		}
	}

	results->rate = ctx->cur_rate;
	results->rtt_req = rtt_req;
}

void smartt_reps_algo(doca_pcc_dev_event_t *event,
		      uint32_t *param,
		      uint32_t *counter,
		      doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		      doca_pcc_dev_results_t *results)
{
	cc_ctxt_smartt_reps_t *ctx = (cc_ctxt_smartt_reps_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = ctx->cur_rate;

	if (counter != NULL)
		counter[SMARTT_REPS_C_EVENTS]++;

	if (unlikely(cur_rate == 0)) {
		smartt_reps_handle_new_flow(event, param, counter, ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_ACK) {
		smartt_reps_handle_ack(event, param, counter, ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_RTT) {
		smartt_reps_handle_rtt(event, param, counter, ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		smartt_reps_handle_tx(event, param, ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		smartt_reps_handle_nack(event, param, counter, ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		smartt_reps_handle_cnp(event, param, counter, ctx, results);
	} else {
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t smartt_reps_set_algo_params(uint32_t param_id_base,
						 uint32_t param_num,
						 const uint32_t *new_param_values,
						 uint32_t *params)
{
	/* Reject writes that would run off the end of the param array: param_id_base and
	 * param_num are bounded individually and as a sum. */
	if ((param_id_base >= SMARTT_REPS_NUM_PARAMS) ||
	    (param_num > SMARTT_REPS_NUM_PARAMS) ||
	    (param_id_base + param_num > SMARTT_REPS_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* Commit the validated values into the live param array: the framework does not copy
	 * them, so returning OK means this callback set them. Range is already enforced by
	 * the min and max registered for each parameter. */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
