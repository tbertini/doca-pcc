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
 * SMaRTT: sender-based ECN + delay windowed congestion control
 *
 * Implementation of the RP (Reaction Point) algorithm from:
 * Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed & Timed
 * Transport with Recycled Entropies", arXiv:2404.01630 (Apr 2024).
 *
 * Reference: Algorithm 1 (four-case loop, §3.2), Algorithm 2 (QuickAdapt, §3.3),
 * Algorithm 3 (FastIncrease, §3.4), Wait-to-Decrease (§3.6.1).
 *
 * SMaRTT combines a delay (fabric RTT) signal and an ECN signal per ACK. On BF3, ACKs
 * carry no timestamps, so the delay signal is the last RTT measured on a separate RTT
 * probe event (Swift's fabric_rtt formula + probe-abort machine, ported verbatim). The
 * ECN signal is a CNP-hold predicate (BF3 delivers rate-limited CNPs, not per-packet
 * marks). We keep cwnd (FXP16 packets) as the control variable and actuate it through
 * the FXP20 rate limiter via the BDP identity (BF3 credits are unbacked), exactly like
 * Swift. REPS (§3.6, per-packet load balancing) is intentionally NOT implemented -- BF3
 * exposes no per-packet entropy hook (see README "REPS is intentionally NOT
 * implemented").
 *
 * See device/rp/smartt_reps/README.md for the signal mapping, the numbered deviations,
 * and the frozen PPCC contract.
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

/* Per-event trace emission, gated by SMARTT_REPS_TRACE_ENABLED(param) (see smartt_reps.h):
 * compiled in unconditionally, emitted only when the trace_on PPCC param is set at
 * runtime (or always under -DSMARTT_REPS_DEBUG). The flush is gated too. */
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

/* Abort time for stuck RTT requests (mirrors rtt_template / TIMELY / Swift). */
#define SMARTT_REPS_ABORT_TIME_NS (300000)

/*
 * Calculate wrap around in case of 32-bit timestamp overflow.
 * Local static copy to avoid a duplicate-symbol clash with the ALWAYS_INLINE
 * diff_with_wrap32 in pcc_common_dev.h under -flto (it doesn't survive LTO when pulled
 * into two translation units). Mirrors TIMELY/Swift diff_with_wrap32.
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

/* Param/counter indices and default/min/max values come from smartt_reps_params.h (the
 * PPCC contract; single source of truth). Description strings are kept const volatile to
 * prevent dead-code elimination by LTO (they are read out-of-band by the host, never
 * dereferenced on-device, so without volatile the contract strings vanish). */
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

	/* Register every parameter from the contract table. min/max/def come from
	 * smartt_reps_params[]; permissions=1 (updatable). Passing the table's min (not a
	 * hardcoded 1) lets fd/mi/trace_on accept a write of 0. */
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

/* ===================== BDP / rate helpers (ported from Swift) ===================== */

/*
 * bdp_fxp16 = line_rate_mbps * base_rtt_ns * 2^16 / (mss_bytes * 8000).
 * cwnd == BDP -> MAX_RATE; cwnd < 1 -> deep sub-line pacing. NB SMaRTT uses base_rtt_ns
 * (brtt) as the BDP RTT, whereas Swift used a separate min_rtt param.
 */
static inline uint32_t smartt_reps_compute_bdp_fxp16(uint32_t *param)
{
	uint64_t num = (uint64_t)param[SMARTT_REPS_P_LINE_RATE_MBPS] * param[SMARTT_REPS_P_BASE_RTT_NS] * (1ull << 16);
	uint64_t den = (uint64_t)param[SMARTT_REPS_P_MSS_BYTES] * 8000ull;
	if (unlikely(den == 0))
		return 0;
	return (uint32_t)(num / den);
}

/*
 * Clamp the actuated rate to [min_rate, MAX_RATE], reporting which bound (if any) hit.
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
 * Convert cwnd (FXP16 packets) -> clamped FXP20 rate via the BDP identity, recording the
 * clamp counters and (optionally) reporting whether either bound hit so the caller can
 * OR the CLAMPED bit into the trace branch code.
 *   rate_fxp20 = clamp((cwnd_fxp16 << 20) / bdp_fxp16, min_rate, MAX_RATE)
 * bdp == 0 (misconfigured params) -> MAX_RATE (fail open to line rate).
 */
static inline uint32_t smartt_reps_cwnd_to_rate(uint32_t *param, uint32_t *counter,
						uint32_t cwnd_fxp16, uint32_t *clamped_flag)
{
	uint32_t bdp_fxp16 = smartt_reps_compute_bdp_fxp16(param);
	uint32_t rate;
	uint32_t clamp_min = 0, clamp_max = 0;

	if (unlikely(bdp_fxp16 == 0))
		rate = DOCA_PCC_DEV_MAX_RATE; /* misconfigured params; fail open to line rate */
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
 * cwnd clamp per §3.5 / Algorithm 1 line 36: cwnd in [min_cwnd, max_cwnd_frac * BDP].
 * The upper cap is computed per event (BDP depends only on params). If the computed cap
 * is below min_cwnd (misconfig), use min_cwnd so the window is never clamped below its
 * own floor.
 */
static inline uint32_t smartt_reps_clamp_cwnd(uint32_t *param, uint32_t cwnd_fxp16)
{
	uint32_t min_cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	uint32_t bdp_fxp16 = smartt_reps_compute_bdp_fxp16(param);
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
 * γ = bdp/reference_bdp (§3.5 "Scaling"): the fi/mi gains are published for a reference
 * fabric (100 Gbps x 12 us, bdp = 150000 bytes) and are multiplied by γ on other
 * fabrics so window growth stays proportional to the BDP. fd/md are NOT scaled (§3.5:
 * always react strongly). bdp_bytes = line_rate_mbps * base_rtt_ns / 8000; saturated
 * against absurd param combinations.
 */
static inline uint32_t smartt_reps_gamma_fxp16(uint32_t *param)
{
	uint64_t bdp_bytes = (uint64_t)param[SMARTT_REPS_P_LINE_RATE_MBPS] *
			     param[SMARTT_REPS_P_BASE_RTT_NS] / 8000ull;
	uint64_t g = (bdp_bytes << 16) / SMARTT_REPS_REF_BDP_BYTES;
	return (g > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)g;
}

/* Resolve the effective fi gain: the param scaled by γ (§3.5 "Scaling"). */
static inline uint32_t smartt_reps_effective_fi(uint32_t *param)
{
	return doca_pcc_dev_fxp_mult(param[SMARTT_REPS_P_FI_FXP16], smartt_reps_gamma_fxp16(param));
}

/* Resolve the effective mi gain: the param, or (mi==0 sentinel) brtt/(trtt-brtt) derived
 * on device (§3.2.4; = 2.0 at trtt = 1.5*brtt; if trtt <= brtt, misconfig, use 2.0),
 * then scaled by γ (§3.5 "Scaling"). */
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

/* ===================== ΔPSN acked-packet tracker ===================== */

/*
 * Resolve the packet count acked/delivered since the last ACK/NACK event from the ΔPSN of
 * first_sn, with the num_coalesced fallback. Updates the ctxt PSN state. Bumps the
 * psn_anomaly counter on an implausible delta (Δ > max(2*cwnd_pkts + 2, 256)) and falls
 * back to num_coalesced (README signal mapping + Alg1 deviation 5).
 */
static inline uint32_t smartt_reps_psn_delta(cc_ctxt_smartt_reps_t *ctx, uint32_t *counter,
					     uint32_t psn, uint32_t ncoal)
{
	uint32_t delta;
	uint32_t cwnd_pkts = ctx->cwnd_fxp16 >> 16;
	uint32_t cap = 2u * cwnd_pkts + 2u;

	if (cap < 256u)
		cap = 256u;

	if (ctx->flags & SMARTT_REPS_FLAG_PSN_VALID) {
		delta = smartt_reps_diff_with_wrap32(psn, ctx->last_ack_psn);
		if (delta > cap) {
			delta = ncoal;
			if (counter != NULL)
				counter[SMARTT_REPS_C_PSN_ANOMALY]++;
		}
	} else {
		delta = ncoal;
	}

	/* Bound the ncoal fallback by the same sanity cap: ncoal is a raw 16-bit field, and
	 * an unbounded size feeds every downstream size-proportional term (FI's
	 * (size<<32)/cwnd in particular). With size <= 2*cwnd_pkts+2 those terms stay in
	 * range by construction. */
	if (delta > cap)
		delta = cap;

	ctx->last_ack_psn = psn;
	ctx->flags |= SMARTT_REPS_FLAG_PSN_VALID;
	return delta;
}

/* ===================== QuickAdapt (Algorithm 2, §3.3) ===================== */

/*
 * QuickAdapt: called per ACK (and per NACK, with src=2). At most one fire per trtt epoch.
 * On fire: cwnd = max(acked, 1 MTU) * qa_scaling; ignore_target = pre-fire cwnd (packets,
 * >=1: Algorithm 2 line 8's bytes_to_ignore = unacked, and the in-flight bytes at fire
 * time ~= the old window); ignored = 0. Epoch boundary always resets qa_epoch_start/acked
 * (Algorithm 2 line 12).
 * Returns 1 if it fired (cwnd changed), 0 otherwise. cwnd is passed/returned by pointer.
 * tsflow is trace-only (SMARTT_REPS_TRACE_TSFLOW(now, flow_tag), precomputed once by the
 * caller): used solely as fmt-21's arg1 below, never read for control decisions.
 */
static inline uint32_t smartt_reps_quick_adapt(uint32_t *param, uint32_t *counter,
					       cc_ctxt_smartt_reps_t *ctx, uint32_t now,
					       uint32_t *cwnd_io, uint32_t src, uint64_t tsflow)
{
	uint32_t trtt = param[SMARTT_REPS_P_TARGET_RTT_NS];
	uint32_t adapted = 0;

	if (smartt_reps_diff_with_wrap32(now, ctx->qa_epoch_start) >= trtt) { /* now >= end */
		/* "end != 0": no fire before the first full epoch has been established. */
		if ((ctx->flags & SMARTT_REPS_FLAG_TRIGGER_QA) &&
		    (ctx->flags & SMARTT_REPS_FLAG_QA_EPOCH_VALID)) {
			/* Algorithm 2 line 8: bytes_to_ignore = unacked. The in-flight bytes at
			 * fire time ~= the window being replaced, so the blackout is derived from
			 * the PRE-fire cwnd (already clamped <= max_cwnd by the previous event,
			 * so it never exceeds one window's worth of packets). Deriving it from
			 * the new post-cut cwnd would end the blackout early and let the device
			 * react to signals from packets sent under the old window. */
			uint32_t prev_cwnd = *cwnd_io;
			uint32_t ignore_target = prev_cwnd >> 16;    /* bytes_to_ignore ~= old cwnd, pkts */
			if (prev_cwnd & 0xFFFFu)                      /* round up */
				ignore_target += 1;
			if (ignore_target < 1)
				ignore_target = 1;
			ctx->packets_to_ignore = (ignore_target > 0xFFFFu) ? 0xFFFFu : (uint16_t)ignore_target;
			ctx->ignored_pkts = 0;

			uint32_t acked = ctx->acked_pkts;
			uint32_t base = (acked >= 1) ? acked : 1;   /* max(acked, mtu) in packets */
			/* cwnd = max(acked,mtu)*qa_scaling in FXP16 packets (§3.3). Explicit u64
			 * with saturation: fxp_mult TRUNCATES its u32 result, and
			 * (base<<16)*qa_scaling exceeds u32 for acked > 32768 with qa_scaling >
			 * 1.0 -- both within the registered param bounds (adversarial-review
			 * finding: acked=32768, qa=2.0 truncated to 0 => floor collapse). */
			uint64_t cwnd64 = (((uint64_t)base << 16) *
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

/* ===================== FastIncrease (Algorithm 3, §3.4) ===================== */

/*
 * FastIncrease: engages when rtt <= brtt*fast_inc_thresh and !ecn. While active (or once
 * the byte counter exceeds cwnd), adds fast_inc_k MTUs per coalesced ACK arrival
 * (scaled by ncoal, deviation 9). Returns 1 if it fired (cwnd changed), 0 otherwise.
 * cwnd passed/returned by pointer. When !engaged, resets the counter and fi_active.
 * tsflow is trace-only (SMARTT_REPS_TRACE_TSFLOW(now, flow_tag), precomputed once by the
 * caller): used solely as fmt-22's arg1 below, never read for control decisions.
 */
static inline uint32_t smartt_reps_fast_increase(uint32_t *param, uint32_t *counter,
						 cc_ctxt_smartt_reps_t *ctx, uint32_t rtt,
						 uint32_t ecn, uint32_t size, uint32_t ncoal,
						 uint32_t *cwnd_io, uint64_t tsflow)
{
	uint32_t brtt = param[SMARTT_REPS_P_BASE_RTT_NS];
	uint32_t thresh = param[SMARTT_REPS_P_FAST_INC_THRESH_FXP16];
	uint32_t rtt_bound = doca_pcc_dev_fxp_mult(brtt, thresh); /* brtt * fast_inc_thresh */

	if ((rtt <= rtt_bound) && !ecn) {
		ctx->fast_inc_count = smartt_reps_sat_add16(ctx->fast_inc_count, size);
		uint32_t count_fxp16 = (uint32_t)ctx->fast_inc_count << 16;
		if ((count_fxp16 > *cwnd_io) || (ctx->flags & SMARTT_REPS_FLAG_FI_ACTIVE)) {
			uint64_t inc = (uint64_t)param[SMARTT_REPS_P_FAST_INC_K_FXP16] * ncoal; /* k MTUs/ACK */
			uint64_t cwnd = (uint64_t)*cwnd_io + inc;
			*cwnd_io = (cwnd > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd;
			ctx->flags |= SMARTT_REPS_FLAG_FI_ACTIVE;
			if (counter != NULL)
				counter[SMARTT_REPS_C_FAST_INC]++;
			/* Cast-then-shift (not shift-then-cast, cf. count_fxp16 above): fast_inc_count
			 * is uint16_t, so "(x & 0xFFFF) << 16" done in promoted-int arithmetic would
			 * shift into the sign bit (UB) -- widen to uint64_t first, same bit pattern. */
			SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_FASTINC, tsflow,
					  (((uint64_t)(ctx->fast_inc_count & 0xFFFF) << 16) | (ncoal & 0xFFFF)),
					  rtt, *cwnd_io, ctx->cur_rate);
			return 1;
		}
		return 0; /* still accumulating: the four cases run this event */
	}

	ctx->fast_inc_count = 0;
	ctx->flags &= ~SMARTT_REPS_FLAG_FI_ACTIVE;
	return 0;
}

/* ===================== four-case window updates (§3.2, FXP16 packets) ===================== */

/*
 * Fair Decrease (§3.2.1 Eq.1): cwnd -= (cwnd/bdp) * fd * size.
 * Underflow-guarded: if the decrement >= cwnd, collapse to min_cwnd. Bumps FD counter.
 */
static inline void smartt_reps_apply_fd(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t bdp, uint32_t size)
{
	uint32_t cwnd = *cwnd_io;
	/* (cwnd/bdp)*fd in FXP16, then * size (integer packets). */
	uint32_t cwnd_over_bdp = (bdp != 0) ? (uint32_t)(((uint64_t)cwnd << 16) / bdp) : 0;
	uint32_t term_fxp16 = doca_pcc_dev_fxp_mult(cwnd_over_bdp, param[SMARTT_REPS_P_FD_FXP16]);
	uint64_t dec = (uint64_t)term_fxp16 * size;

	if (dec >= cwnd)
		*cwnd_io = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	else
		*cwnd_io = cwnd - (uint32_t)dec;

	if (counter != NULL)
		counter[SMARTT_REPS_C_FD]++;
}

/*
 * Multiplicative Decrease (§3.2.2 Eq.2), then additionally applies FD (§3.2.2 text):
 *   dec = ((rtt-trtt)/rtt) * md * size, capped at size MTU; cwnd -= dec; then FD.
 * Requires rtt > trtt > 0 (guaranteed by the caller's branch condition). Underflow-guard.
 */
static inline void smartt_reps_apply_md(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t rtt, uint32_t trtt,
					uint32_t bdp, uint32_t size)
{
	uint32_t cwnd = *cwnd_io;
	uint32_t ratio = (rtt != 0) ? (uint32_t)((((uint64_t)(rtt - trtt)) << 16) / rtt) : 0; /* (rtt-trtt)/rtt FXP16 */
	uint32_t md_term = doca_pcc_dev_fxp_mult(ratio, param[SMARTT_REPS_P_MD_FXP16]);        /* * md */
	uint64_t dec = (uint64_t)md_term * size;                                               /* FXP16 pkts */
	uint64_t cap = (uint64_t)size << 16;                                                   /* at most size */
	if (dec > cap)
		dec = cap;

	if (dec >= cwnd)
		*cwnd_io = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	else
		*cwnd_io = cwnd - (uint32_t)dec;

	if (counter != NULL)
		counter[SMARTT_REPS_C_MD]++;

	/* §3.2.2: MD additionally applies FD. */
	smartt_reps_apply_fd(param, counter, cwnd_io, bdp, size);
}

/*
 * Fair Increase (§3.2.3 Eq.3): cwnd += (size/cwnd) * mtu * fi, with fi γ-scaled (§3.5).
 * mtu == 1.0 == 65536, so (size/cwnd)*mtu = (size<<32)/cwnd in FXP16. Bumps FI counter.
 */
static inline void smartt_reps_apply_fi(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t size)
{
	uint32_t cwnd = *cwnd_io;
	/* (size/cwnd)*mtu in FXP16, saturated: with min_cwnd configured sub-MTU (registered
	 * floor 66 = 0.001 pkt) the quotient can exceed u32; saturate instead of silently
	 * truncating (final clamp_cwnd bounds the result either way). */
	uint64_t ratio64 = (cwnd != 0) ? (((uint64_t)size << 32) / cwnd) : 0;
	uint32_t ratio_mtu = (ratio64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)ratio64;
	uint32_t inc_fxp16 = doca_pcc_dev_fxp_mult(ratio_mtu, smartt_reps_effective_fi(param));
	uint64_t cwnd_new = (uint64_t)cwnd + inc_fxp16;
	*cwnd_io = (cwnd_new > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd_new;

	if (counter != NULL)
		counter[SMARTT_REPS_C_FI]++;
}

/*
 * Multiplicative Increase (§3.2.4 Eq.4), then additionally applies FI (§3.2.4 text):
 *   inc = min( ((trtt-rtt)/rtt) * (size/cwnd) * mtu * mi , size MTU ); cwnd += inc; then FI.
 * Requires trtt > rtt (caller's branch), rtt > 0. mi is the effective (possibly derived,
 * γ-scaled) gain. Bumps MI counter.
 */
static inline void smartt_reps_apply_mi(uint32_t *param, uint32_t *counter,
					uint32_t *cwnd_io, uint32_t rtt, uint32_t trtt,
					uint32_t mi, uint32_t size)
{
	uint32_t cwnd = *cwnd_io;
	uint32_t ratio = (rtt != 0) ? (uint32_t)((((uint64_t)(trtt - rtt)) << 16) / rtt) : 0; /* (trtt-rtt)/rtt FXP16 */
	/* (size/cwnd)*mtu in FXP16, saturated (same sub-MTU min_cwnd rationale as apply_fi;
	 * Eq.4's min(.., size) cap bounds the final increment regardless). */
	uint64_t socm64 = (cwnd != 0) ? (((uint64_t)size << 32) / cwnd) : 0;
	uint32_t size_over_cwnd_mtu = (socm64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)socm64;
	/* fxp_mult3(ratio, size_over_cwnd_mtu, mi): chain two FXP16 multiplies. */
	uint32_t t = doca_pcc_dev_fxp_mult(ratio, size_over_cwnd_mtu);
	uint32_t inc_fxp16 = doca_pcc_dev_fxp_mult(t, mi);
	uint64_t inc = inc_fxp16;
	uint64_t cap = (uint64_t)size << 16; /* at most size */
	if (inc > cap)
		inc = cap;
	uint64_t cwnd_new = (uint64_t)cwnd + inc;
	*cwnd_io = (cwnd_new > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)cwnd_new;

	if (counter != NULL)
		counter[SMARTT_REPS_C_MI]++;

	/* §3.2.4: MI is followed by FI. */
	smartt_reps_apply_fi(param, counter, cwnd_io, size);
}

/* ===================== event handlers ===================== */

/*
 * New flow (cur_rate == 0 sentinel): cwnd = init_cwnd (param 0 => bdp_fxp16), rtt = brtt,
 * avg_wtd = 0, PSN/QA/FI state invalid/zero. Actuate, request first RTT probe.
 */
static inline void smartt_reps_handle_new_flow(doca_pcc_dev_event_t *event,
					       uint32_t *param,
					       uint32_t *counter,
					       cc_ctxt_smartt_reps_t *ctx,
					       doca_pcc_dev_results_t *results)
{
	uint32_t now = doca_pcc_dev_get_timestamp(event);
	/* arg1 for every trace emitted below (see SMARTT_REPS_TRACE_TSFLOW, smartt_reps.h). */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t init_cwnd = param[SMARTT_REPS_P_INIT_CWND_FXP16];
	uint32_t bdp = smartt_reps_compute_bdp_fxp16(param);
	uint32_t cwnd = (init_cwnd != 0) ? init_cwnd : bdp;
	uint32_t clamped = 0;
	uint32_t rate;

	if (cwnd == 0) /* both init_cwnd==0 and bdp==0 (misconfig): fall back to 1 MTU */
		cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];

	rate = smartt_reps_cwnd_to_rate(param, counter, cwnd, &clamped);

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
	ctx->flags = 0; /* trigger_qa / fi_active / psn_valid / qa_epoch_valid / cnp_seen / probe all clear */
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
 * ACK event (DOCA_PCC_DEV_EVNT_ROCE_ACK = 4) -- THE MAIN LOOP (Algorithm 1, §3.2-3.6.1).
 * Consumes the stored fabric RTT (probe events keep it fresh) and the CNP-hold ECN
 * predicate. See the README control-law block for the exact ordering.
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
	/* arg1 for every trace emitted below (see SMARTT_REPS_TRACE_TSFLOW, smartt_reps.h).
	 * Computed once and threaded into quick_adapt()/fast_increase() so every trace fired
	 * from this single event -- fmt 20/21/22/23 -- carries the same tsflow. */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t psn = doca_pcc_dev_get_roce_ack_first_sn(event);
	doca_pcc_dev_ack_nack_cnp_extra_t extra = doca_pcc_dev_get_ack_nack_cnp_extra(event);
	uint32_t ncoal = extra.num_coalesced;
	if (ncoal == 0)
		ncoal = 1; /* treat 0 as 1 */

	uint32_t size = smartt_reps_psn_delta(ctx, counter, psn, ncoal);

	uint32_t rtt  = ctx->last_rtt_ns;
	uint32_t trtt = param[SMARTT_REPS_P_TARGET_RTT_NS];
	uint32_t bdp  = smartt_reps_compute_bdp_fxp16(param);

	/* p.ecn := CNP arrived within the hold window (README signal mapping). */
	uint32_t ecn = 0;
	if (ctx->flags & SMARTT_REPS_FLAG_CNP_SEEN) {
		uint32_t since_cnp = smartt_reps_diff_with_wrap32(now, ctx->last_cnp_ts);
		ecn = (since_cnp <= param[SMARTT_REPS_P_ECN_CNP_HOLD_NS]) ? 1 : 0;
	}

	/* Alg1 lines 4-5: accumulate acked (QA epoch) and ignored (post-QA blackout). */
	ctx->acked_pkts = smartt_reps_sat_add16(ctx->acked_pkts, size);
	ctx->ignored_pkts = smartt_reps_sat_add16(ctx->ignored_pkts, size);

	/* Raw ACK-signal trace (fmt 23): answers "do ACK events arrive, and is ΔPSN sane?".
	 * event_sn (doca_pcc_dev_get_sn) is no longer sampled -- dropped from the format,
	 * see README (sn ~= first_sn + delta, derivable from the ΔPSN reported here anyway). */
	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_SIG, tsflow, psn,
			  ((uint64_t)((size & 0xFFFF) << 16) | (ncoal & 0xFFFF)), ctx->avg_wtd_fxp16, 0);

	/* Alg1 lines 8-10: post-QuickAdapt blackout (§3.3). While still ignoring the last
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

	/* WTD §3.6.1: avg_wtd = alpha*(ecn?1:0) + (1-alpha)*avg_wtd, once per ACK event.
	 * Computed in uint32 FXP16, then capped to 65535 for the uint16 field. */
	{
		uint32_t alpha = param[SMARTT_REPS_P_WTD_ALPHA_FXP16];
		uint32_t one_minus_alpha = (alpha <= SMARTT_REPS_FXP16_ONE) ? (SMARTT_REPS_FXP16_ONE - alpha) : 0;
		uint32_t ecn_term = ecn ? alpha : 0; /* alpha*1 or alpha*0 in FXP16 */
		uint32_t decay = doca_pcc_dev_fxp_mult(one_minus_alpha, ctx->avg_wtd_fxp16);
		uint32_t avg = ecn_term + decay;
		ctx->avg_wtd_fxp16 = (avg > 0xFFFFu) ? 0xFFFFu : (uint16_t)avg;
	}
	uint32_t can_decrease = (ctx->avg_wtd_fxp16 >= param[SMARTT_REPS_P_WTD_THRESH_FXP16]);

	uint32_t cwnd = ctx->cwnd_fxp16;
	uint32_t branch;
	uint32_t edge_bits = (ecn ? SMARTT_REPS_BRANCH_ECN_BIT : 0) |
			     (can_decrease ? SMARTT_REPS_BRANCH_CAN_DEC_BIT : 0);

	/* Alg1: QuickAdapt (always called) then FastIncrease (always called) -- the README
	 * control-law block evaluates BOTH before the "if adp or finc: goto clamp" test.
	 * fast_increase must run every ACK so its fast_inc_count/fi_active state machine
	 * stays consistent even when QA also fired. If either fires, skip the four cases and
	 * go straight to clamp+actuate (deviation 7). Both mutate cwnd through the pointer,
	 * so when both fire FI stacks on the QA-reset cwnd (paper Algorithm 1 order). The
	 * fmt20 branch code reports QA_ADAPTED in preference to FASTINC (QA is the
	 * more-significant severe-congestion response). */
	uint32_t adp = smartt_reps_quick_adapt(param, counter, ctx, now, &cwnd, 1 /* src=ack */, tsflow);
	uint32_t finc = smartt_reps_fast_increase(param, counter, ctx, rtt, ecn, size, ncoal, &cwnd, tsflow);

	if (adp) {
		branch = SMARTT_REPS_BRANCH_QA_ADAPTED;
	} else if (finc) {
		branch = SMARTT_REPS_BRANCH_FASTINC;
	} else {
		/* --- four-case control loop (§3.2) --- */
		if (ecn && (rtt <= trtt) && can_decrease) {
			smartt_reps_apply_fd(param, counter, &cwnd, bdp, size);   /* §3.2.1 */
			branch = SMARTT_REPS_BRANCH_FD;
		} else if (ecn && (rtt > trtt) && can_decrease) {
			smartt_reps_apply_md(param, counter, &cwnd, rtt, trtt, bdp, size); /* §3.2.2 (MD+FD) */
			branch = SMARTT_REPS_BRANCH_MD;
		} else if (!ecn && (rtt > trtt)) {
			smartt_reps_apply_fi(param, counter, &cwnd, size);        /* §3.2.3 */
			branch = SMARTT_REPS_BRANCH_FI;
		} else if (!ecn && (rtt <= trtt)) {
			uint32_t mi = smartt_reps_effective_mi(param);
			smartt_reps_apply_mi(param, counter, &cwnd, rtt, trtt, mi, size); /* §3.2.4 (MI+FI) */
			branch = SMARTT_REPS_BRANCH_MI;
		} else {
			/* ecn && !can_decrease: WTD blocked the decrease -- no window change. */
			branch = SMARTT_REPS_BRANCH_WTD_BLOCKED;
			if (counter != NULL)
				counter[SMARTT_REPS_C_WTD_BLOCKED]++;
		}
	}

	/* Alg1 line 36 + §3.5: clamp cwnd to [min_cwnd, max_cwnd_frac * BDP]. */
	cwnd = smartt_reps_clamp_cwnd(param, cwnd);
	ctx->cwnd_fxp16 = cwnd;

	/* Actuate: cwnd -> FXP20 rate (BDP). */
	uint32_t clamped = 0;
	uint32_t rate = smartt_reps_cwnd_to_rate(param, counter, cwnd, &clamped);
	if (clamped)
		branch |= SMARTT_REPS_BRANCH_CLAMPED_BIT;
	branch |= edge_bits;

	ctx->cur_rate = rate;
	results->rate = rate;
	results->rtt_req = 0;

	SMARTT_REPS_TRACE(param, SMARTT_REPS_TRACE_FMT_ACK, tsflow, branch, rtt, cwnd, rate);
	SMARTT_REPS_TRACE_FLUSH(param);
}

/*
 * NACK event (DOCA_PCC_DEV_EVNT_ROCE_NACK = 5) = trimming/timeout surrogate (Algorithm 1
 * lines 28-34). NACK first_sn is the receiver's expected PSN, so its ΔPSN counts
 * delivered pkts (acked += Δ, ignored += Δ keeps QA's acked measurement honest under
 * go-back-N). cwnd -= 1 MTU (underflow-guarded); trigger_qa = true; if ignored >=
 * ignore_target, QuickAdapt(now).
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
	/* arg1 for the fmt-24 trace below and threaded into quick_adapt() for fmt-21 (see
	 * SMARTT_REPS_TRACE_TSFLOW, smartt_reps.h). */
	uint64_t tsflow = SMARTT_REPS_TRACE_ENABLED(param) ?
			  SMARTT_REPS_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;
	uint32_t psn = doca_pcc_dev_get_roce_ack_first_sn(event);
	doca_pcc_dev_ack_nack_cnp_extra_t extra = doca_pcc_dev_get_ack_nack_cnp_extra(event);
	uint32_t ncoal = extra.num_coalesced;
	if (ncoal == 0)
		ncoal = 1;

	uint32_t delta = smartt_reps_psn_delta(ctx, counter, psn, ncoal);
	ctx->acked_pkts = smartt_reps_sat_add16(ctx->acked_pkts, delta);
	ctx->ignored_pkts = smartt_reps_sat_add16(ctx->ignored_pkts, delta);

	/* line 29: cwnd -= 1 MTU (one lost packet's p.size); underflow-guard to min_cwnd. */
	uint32_t cwnd = ctx->cwnd_fxp16;
	uint32_t min_cwnd = param[SMARTT_REPS_P_MIN_CWND_FXP16];
	if (cwnd > SMARTT_REPS_MTU_FXP16 + min_cwnd)
		cwnd -= SMARTT_REPS_MTU_FXP16;
	else
		cwnd = min_cwnd;

	/* line 30: trigger_qa = true. */
	ctx->flags |= SMARTT_REPS_FLAG_TRIGGER_QA;

	/* lines 32-34: if ignored >= ignore_target, QuickAdapt now (src=2). */
	if (ctx->ignored_pkts >= ctx->packets_to_ignore)
		(void)smartt_reps_quick_adapt(param, counter, ctx, now, &cwnd, 2 /* src=nack */, tsflow);

	/* clamp + actuate; rtt_req = 0. */
	cwnd = smartt_reps_clamp_cwnd(param, cwnd);
	ctx->cwnd_fxp16 = cwnd;

	uint32_t clamped = 0;
	uint32_t rate = smartt_reps_cwnd_to_rate(param, counter, cwnd, &clamped);
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
 * CNP event (DOCA_PCC_DEV_EVNT_ROCE_CNP = 2): record the arrival timestamp for the
 * ECN-hold predicate. Decreases happen in the ACK loop via p.ecn; rate unchanged here.
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
 * RTT event (DOCA_PCC_DEV_EVNT_RTT = 6): store the fresh fabric RTT for the ACK loop to
 * consume, reset the probe-abort state, re-arm rtt_req=1. Rate unchanged. SMaRTT does not
 * adjust cwnd on probe events (the paper has no probe events; this is the central cadence
 * adaptation -- README Deviation 2). fabric_rtt uses Swift's exact formula with the
 * t2/t4==0 fallback.
 */
static inline void smartt_reps_handle_rtt(doca_pcc_dev_event_t *event,
					  uint32_t *param,
					  uint32_t *counter,
					  cc_ctxt_smartt_reps_t *ctx,
					  doca_pcc_dev_results_t *results)
{
	if (counter != NULL)
		counter[SMARTT_REPS_C_RTT_EVENTS]++;

	/* Drop spurious RTT events not matching a probe we are waiting on (aborted probe).
	 * Mirrors rtt_template / TIMELY / Swift. */
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
	/* resp_send/req_recv are populated by the NP for RTT probes; if absent (0), fall
	 * back to total_rtt (== what TIMELY uses). See README Deviation 2. */
	if (likely(resp_send != 0 && req_recv != 0))
		np_turnaround = smartt_reps_diff_with_wrap32(resp_send, req_recv);
	uint32_t fabric_rtt = (np_turnaround < total_rtt) ? (total_rtt - np_turnaround) : total_rtt;

	ctx->last_rtt_ns = fabric_rtt;

	results->rate = ctx->cur_rate;      /* rate unchanged on probe events */
	results->rtt_req = 1;               /* re-arm next RTT probe */
	ctx->tx_since_probe_req = 1;
}

/*
 * TX event (DOCA_PCC_DEV_EVNT_ROCE_TX = 3): the RTT-probe abort/retry state machine,
 * ported verbatim from Swift/rtt_template/TIMELY, adapted to this ctxt layout
 * (rtt_probe_outstanding flag, probe_start_ts, abort_cnt, tx_since_probe_req). No cwnd
 * change. Without this machine a single dropped RTT response stalls the flow forever.
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
		if (unlikely((probe_out_time_ns > ((uint32_t)SMARTT_REPS_ABORT_TIME_NS << ctx->abort_cnt)) ||
			     (ctx->tx_since_probe_req > 2))) {
			rtt_req = 1;
			/* Cap the backoff exponent: 300000 << 14 wraps u32 and << 32 is UB
			 * (latent in the swift/timely originals; abort_cnt only ever grows).
			 * 12 caps the abort threshold at ~1.2 s. */
			if ((probe_out_time_ns > ((uint32_t)SMARTT_REPS_ABORT_TIME_NS << ctx->abort_cnt)) &&
			    (ctx->abort_cnt < 12))
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
	/* Reject writes that would run off the end of the param array. param_id_base and
	 * param_num are bounded individually AND together: param_id_base+param_num must stay
	 * within SMARTT_REPS_NUM_PARAMS, otherwise the copy below writes OOB. */
	if ((param_id_base >= SMARTT_REPS_NUM_PARAMS) ||
	    (param_num > SMARTT_REPS_NUM_PARAMS) ||
	    (param_id_base + param_num > SMARTT_REPS_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* Commit the validated values into the live param array. The framework does NOT
	 * auto-copy: per the doca_pcc_dev_user_set_algo_params contract, returning OK means
	 * "the callback set them" (FAIL means "nothing changed"). Without this loop, the
	 * array returned by doca_pcc_dev_get_algo_params() keeps the init-time defaults, so
	 * PPCC writes read back correctly (register image) yet never reach the running
	 * algorithm. Range is already enforced by the min/max registered in
	 * smartt_reps_init(). */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
