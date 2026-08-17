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
 * Swift-CX8: delay-based, window AIMD congestion control, actuated as a real window
 *
 * Implementation of the RP (Reaction Point) algorithm from:
 * Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
 * Datacenter", SIGCOMM 2020 (DOI 10.1145/3387514.3406591).
 *
 * Reference: Algorithm 1 (§3.2) and target-delay scaling (§3.5).
 *
 * ============================ READ THIS FIRST ============================
 * This variant exists to be as close to the paper as the DOCA PCC model allows UNDER
 * THE ASSUMPTION that doca_pcc_dev_results_t.credits / .reload_credits actually reach
 * the hardware. On every DOCA 2.10 chip they do NOT: disassembly of libdoca_pcc_dev.a
 * shows the post-algorithm glue loading only results->rate (offset 0) and
 * results->rtt_req (offset 4), plus probe_type_slot (offset 8) on ConnectX-8; offsets
 * 12 and 16 (credits, reload_credits) are never read. Third-party confirmation: Huang
 * et al., "SwCC", USENIX ATC 2025, §3.4 -- "PCC only allows developers to adjust
 * per-flow rates and cannot support credit-based or window-based CCAs".
 *
 * Consequence: on BlueField-3 the credit writes below are silent no-ops, and because
 * this variant deliberately opens the rate limiter (rate = MAX_RATE) whenever
 * cwnd >= 1 packet -- letting the window, not the limiter, govern -- it degenerates to
 * an UNCONTROLLED SENDER on current silicon. It is a reference implementation and a
 * thesis artefact, NOT a benchmark target. For measurements on this hardware use
 * device/rp/swift/, which maps the same cwnd onto the rate limiter via the BDP
 * identity. See README.md.
 * =========================================================================
 *
 * Swift is a simplification of TIMELY: instead of reacting to the RTT *gradient*, it
 * runs AIMD on a congestion *window* (cwnd, in fractional packets) against an
 * *absolute*, flow-scaled target delay. Two things make this variant more faithful to
 * Algorithm 1 than device/rp/swift/:
 *   1. The window is actuated AS a window (results->credits), not converted to a rate.
 *      The rate limiter is opened wide and transmission is ACK-clocked, which is what
 *      the paper assumes. Below one packet the window cannot be expressed in credits,
 *      so the FXP20 rate takes over and implements the paper's
 *      pacing_delay = rtt/cwnd regime (Algorithm 1 lines 29-32).
 *   2. AIMD runs per ACK event with the paper's (ai/cwnd)*num_acked increase, instead
 *      of one +ai step per RTT-probe event.
 *
 * What remains deviant, unavoidably: the delay signal still comes from explicit RTT
 * probes (responder-reflected t2/t4, sender decomposes), so it refreshes once per probe
 * round-trip rather than on every ACK, and the ACK handler regulates on the most recent
 * probe sample. v1 is also FABRIC-ONLY: one window (fcwnd) driven by the fabric RTT.
 * The endpoint window (ecwnd, paper §3.3) is deferred (see FIXMEs); ecwnd/endpoint
 * state in the context is reserved but unused.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "swift_cx8_ctxt.h"
#include "swift_cx8_params.h"
#include "swift_cx8_algo_params.h"
#include "swift_cx8.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Per-event trace emission, gated by SWIFT_CX8_TRACE_ENABLED(param) (see swift_cx8.h):
 * compiled in unconditionally, emitted only when the trace_on PPCC param is set at
 * runtime (or always under -DSWIFT_CX8_DEBUG). The flush is gated too. */
#define SWIFT_CX8_TRACE(param, fmt_id, a1, a2, a3, a4, a5) \
	do { \
		if (SWIFT_CX8_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_5(fmt_id, (uint64_t)(a1), (uint64_t)(a2), \
					     (uint64_t)(a3), (uint64_t)(a4), (uint64_t)(a5)); \
	} while (0)
#define SWIFT_CX8_TRACE_FLUSH(param) \
	do { \
		if (SWIFT_CX8_TRACE_ENABLED(param)) \
			doca_pcc_dev_trace_flush(); \
	} while (0)

/* Abort time for stuck RTT requests (mirrors rtt_template / TIMELY). */
#define SWIFT_CX8_ABORT_TIME_NS (300000)

/*
 * Calculate wrap around in case of 32-bit timestamp overflow.
 * Local static copy to avoid a duplicate-symbol clash with the ALWAYS_INLINE
 * diff_with_wrap32 in pcc_common_dev.h under -flto (it doesn't survive LTO when pulled
 * into two translation units). Mirrors TIMELY's timely_diff_with_wrap32.
 */
static inline uint32_t swift_cx8_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* Param/counter indices and default/min/max values come from swift_cx8_params.h (the PPCC
 * contract; single source of truth). Description strings are kept const volatile to
 * prevent dead-code elimination by LTO (they are read out-of-band by the host, never
 * dereferenced on-device, so without volatile the contract strings vanish). */
const volatile char swift_cx8_desc[] = "Swift-CX8 v1.0 credit-window (Kumar et al. SIGCOMM 2020)";
static const volatile char swift_cx8_p_base_target_desc[]   = "BASE_TARGET_NS, base fabric target delay (ns)";
static const volatile char swift_cx8_p_fs_range_desc[]      = "FS_RANGE_NS, max additional target for small flows (ns)";
static const volatile char swift_cx8_p_fs_min_cwnd_desc[]   = "FS_MIN_CWND, low knee for flow scaling (FXP16 pkts)";
static const volatile char swift_cx8_p_fs_max_cwnd_desc[]   = "FS_MAX_CWND, high knee for flow scaling (FXP16 pkts)";
static const volatile char swift_cx8_p_num_hops_desc[]      = "NUM_HOPS, fixed hop count (BF3 TTL unreadable)";
static const volatile char swift_cx8_p_hop_scale_desc[]     = "HOP_SCALE_NS, per-hop target add (ns, 0 on BF3)";
static const volatile char swift_cx8_p_ai_desc[]            = "AI, additive increment per RTT of acks (FXP16 pkts)";
static const volatile char swift_cx8_p_md_beta_desc[]       = "MD_BETA, multiplicative-decrease gain (FXP16)";
static const volatile char swift_cx8_p_max_mdf_desc[]       = "MAX_MDF, max multiplicative decrease factor (FXP16)";
static const volatile char swift_cx8_p_min_cwnd_desc[]      = "MIN_CWND, cwnd floor (FXP16 pkts)";
static const volatile char swift_cx8_p_max_cwnd_desc[]      = "MAX_CWND, cwnd ceiling (FXP16 pkts)";
static const volatile char swift_cx8_p_init_cwnd_desc[]     = "INIT_CWND, new-flow starting window (FXP16 pkts)";
static const volatile char swift_cx8_p_retx_reset_desc[]    = "RETX_RESET_THRESH, consecutive NACKs to reset cwnd to min";
static const volatile char swift_cx8_p_mss_desc[]           = "MSS_BYTES, segment size for BDP (bytes)";
static const volatile char swift_cx8_p_min_rtt_desc[]       = "MIN_RTT_NS, base RTT for BDP and once-per-RTT guard (ns)";
static const volatile char swift_cx8_p_line_rate_desc[]     = "LINE_RATE_MBPS, NIC line rate for BDP (Mbps)";
static const volatile char swift_cx8_p_min_rate_desc[]      = "MIN_RATE, rate floor (FXP20 of line rate)";
static const volatile char swift_cx8_p_ep_target_desc[]     = "ENDPOINT_TARGET_NS, endpoint window target (ns); phase 2, inert";
static const volatile char swift_cx8_p_ep_ewma_desc[]       = "ENDPOINT_EWMA, endpoint-delay EWMA weight (FXP16); phase 2, inert";
static const volatile char swift_cx8_p_trace_on_desc[]      = "TRACE_ON, runtime trace toggle (0/1); always on if built -DSWIFT_CX8_DEBUG";
static const volatile char swift_cx8_c_events_desc[]        = "EVENTS, total events dispatched";
static const volatile char swift_cx8_c_ai_desc[]            = "AI, additive-increase applications";
static const volatile char swift_cx8_c_md_desc[]            = "MD, multiplicative-decrease applications";
static const volatile char swift_cx8_c_delay_lt_desc[]      = "DELAY_LT_TARGET, ACK events with delay < target";
static const volatile char swift_cx8_c_delay_gt_desc[]      = "DELAY_GT_TARGET, ACK events with delay >= target";
static const volatile char swift_cx8_c_cwnd_lt1_desc[]      = "CWND_BELOW_ONE, events leaving cwnd < 1 pkt (pacing)";
static const volatile char swift_cx8_c_fs_sat_desc[]        = "FLOW_SCALE_SATURATED, fs_term clamped to 0 or fs_range";
static const volatile char swift_cx8_c_cd_block_desc[]      = "CAN_DECREASE_BLOCKED, MD blocked by once-per-RTT guard";
static const volatile char swift_cx8_c_clamp_min_desc[]     = "RATE_CLAMPED_MIN, rate clamped up to min floor";
static const volatile char swift_cx8_c_clamp_max_desc[]     = "RATE_CLAMPED_MAX, rate clamped down to MAX_RATE";
static const volatile char swift_cx8_c_nack_md_desc[]       = "NACK_MD, NACK-triggered multiplicative decrease";
static const volatile char swift_cx8_c_nack_reset_desc[]    = "NACK_RESET, NACK retx threshold reset cwnd to min";
static const volatile char swift_cx8_c_rtt_drop_desc[]       = "RTT_DROPPED_UNSTAMPED, RTT samples dropped (responder t2/t4 missing/inconsistent)";

void swift_cx8_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = SWIFT_CX8_ALGO_ID;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(swift_cx8_desc);
	algo_def.algo_desc_addr = (uint64_t)swift_cx8_desc;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, SWIFT_CX8_NUM_PARAMS, SWIFT_CX8_NUM_COUNTERS);

	/* Register every parameter from the contract table. min/max/def come from
	 * swift_cx8_params[]; permissions=1 (updatable). Passing the table's min (not a
	 * hardcoded 1) lets hop_scale and trace_on accept a write of 0. */
#define SWIFT_CX8_REG_PARAM(idx, desc) \
	doca_pcc_dev_algo_init_param(algo_idx, (idx), swift_cx8_params[(idx)].def, swift_cx8_params[(idx)].max, \
				     swift_cx8_params[(idx)].min, 1, sizeof(desc), (uint64_t)(desc))

	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_BASE_TARGET_NS,      swift_cx8_p_base_target_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_FS_RANGE_NS,         swift_cx8_p_fs_range_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_FS_MIN_CWND,         swift_cx8_p_fs_min_cwnd_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_FS_MAX_CWND,         swift_cx8_p_fs_max_cwnd_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_NUM_HOPS,            swift_cx8_p_num_hops_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_HOP_SCALE_NS,        swift_cx8_p_hop_scale_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_AI_FXP16,            swift_cx8_p_ai_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MD_BETA_FXP16,       swift_cx8_p_md_beta_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MAX_MDF_FXP16,       swift_cx8_p_max_mdf_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MIN_CWND,            swift_cx8_p_min_cwnd_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MAX_CWND,            swift_cx8_p_max_cwnd_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_INIT_CWND,           swift_cx8_p_init_cwnd_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_RETX_RESET_THRESH,   swift_cx8_p_retx_reset_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MSS_BYTES,           swift_cx8_p_mss_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MIN_RTT_NS,          swift_cx8_p_min_rtt_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_LINE_RATE_MBPS,      swift_cx8_p_line_rate_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_MIN_RATE_FXP20,      swift_cx8_p_min_rate_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_ENDPOINT_TARGET_NS,  swift_cx8_p_ep_target_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_ENDPOINT_EWMA_FXP16, swift_cx8_p_ep_ewma_desc);
	SWIFT_CX8_REG_PARAM(SWIFT_CX8_P_TRACE_ON,            swift_cx8_p_trace_on_desc);
#undef SWIFT_CX8_REG_PARAM

#define SWIFT_CX8_REG_COUNTER(idx, desc) \
	doca_pcc_dev_algo_init_counter(algo_idx, (idx), UINT32_MAX, 2, sizeof(desc), (uint64_t)(desc))

	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_EVENTS,             swift_cx8_c_events_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_AI,                 swift_cx8_c_ai_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_MD,                 swift_cx8_c_md_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_DELAY_LT_TARGET,    swift_cx8_c_delay_lt_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_DELAY_GT_TARGET,    swift_cx8_c_delay_gt_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_CWND_BELOW_ONE,     swift_cx8_c_cwnd_lt1_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_FLOW_SCALE_SAT,     swift_cx8_c_fs_sat_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_CAN_DECREASE_BLOCK, swift_cx8_c_cd_block_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_RATE_CLAMPED_MIN,   swift_cx8_c_clamp_min_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_RATE_CLAMPED_MAX,   swift_cx8_c_clamp_max_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_NACK_MD,            swift_cx8_c_nack_md_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_NACK_RESET,         swift_cx8_c_nack_reset_desc);
	SWIFT_CX8_REG_COUNTER(SWIFT_CX8_C_RTT_DROPPED_UNSTAMPED, swift_cx8_c_rtt_drop_desc);
#undef SWIFT_CX8_REG_COUNTER
}

/*
 * Reciprocal square root in FXP16: returns (1/sqrt(x)) * 2^16 for x given in FXP16.
 *
 * Uses the identity 1/sqrt(x) = 2^(-0.5 * log2(x)) via the DPA's FXP16 log2/pow2
 * intrinsics (no float, no sqrt on the DPA). This is the single most precision-sensitive
 * numeric piece; it underpins the flow-scaled target (paper §3.5).
 *
 * Hand-check: x=0.5pkt(32768)->1.414(92682); x=1pkt(65536)->1.0(65536);
 *             x=16pkt(1048576)->0.25(16384); x=0.001pkt(66)->~31.6(~2.07e6).
 *
 * Validated on silicon 2026-08-17 (BF3, DOCA 2.10) via the device/rp/swift/ build, which
 * runs this same routine on the same intrinsics: the traced fs_term (format 17) matches
 * the ideal float curve within 19 ns of fs_range = 15 us (0.13%) over invsqrt inputs
 * 0.1-1024 pkt, including sub-unity fractional cwnd -- so the precision and sign
 * behaviour of __dpa_fxp_log2/__dpa_fxp_pow2 holds over that domain. If a future input
 * range misbehaves, fall back to a Newton step or a cwnd-bucketed LUT.
 */
static inline uint32_t swift_cx8_recip_sqrt_fxp16(uint32_t x_fxp16)
{
	if (x_fxp16 == 0)
		return 0; /* guard log2(0); callers keep cwnd >= min_cwnd > 0 */
	int32_t log2_fxp16 = (int32_t)doca_pcc_dev_fxp_log2(x_fxp16);
	int32_t neg_half_log2 = -(log2_fxp16 / 2); /* -0.5 * log2(x), FXP16 (signed) */
	return doca_pcc_dev_fxp_power2((uint32_t)neg_half_log2);
}

/*
 * Flow-scaled target delay (paper §3.5, Table 2):
 *   t = base_target + num_hops*hop_scale + max(0, min(alpha/sqrt(fcwnd) + beta, fs_range))
 * with alpha = fs_range / (1/sqrt(fs_min_cwnd) - 1/sqrt(fs_max_cwnd)),
 *      beta  = -alpha / sqrt(fs_max_cwnd).
 *
 * Since beta = -alpha*invsqrt(fs_max), the scaled term collapses to a clean linear
 * interpolation of fs_range over the 1/sqrt(cwnd) axis between the two knees -- no
 * explicit alpha/beta (and no division of fs_range by a tiny denominator):
 *   fs_term = fs_range * (invsqrt(cwnd) - invsqrt(fs_max)) / (invsqrt(fs_min) - invsqrt(fs_max))
 * clamped to [0, fs_range]. cwnd<=fs_min -> fs_range; cwnd>=fs_max -> 0.
 *
 * BF3 note: doca_pcc_dev_get_ttl_hoplimit() returns 0, so num_hops is a fixed param and
 * hop_scale defaults to 0 (per-hop term inert). See swift_cx8_algo_params.h.
 */
static inline uint32_t swift_cx8_flow_scaled_target(uint32_t *param, uint32_t cwnd_fxp16, uint32_t *counter)
{
	uint32_t base      = param[SWIFT_CX8_P_BASE_TARGET_NS];
	uint32_t fs_range  = param[SWIFT_CX8_P_FS_RANGE_NS];
	uint32_t fs_min    = param[SWIFT_CX8_P_FS_MIN_CWND];
	uint32_t fs_max    = param[SWIFT_CX8_P_FS_MAX_CWND];
	uint32_t hop_term  = param[SWIFT_CX8_P_NUM_HOPS] * param[SWIFT_CX8_P_HOP_SCALE_NS];

	uint32_t invsqrt_cwnd = swift_cx8_recip_sqrt_fxp16(cwnd_fxp16);
	uint32_t invsqrt_min  = swift_cx8_recip_sqrt_fxp16(fs_min);
	uint32_t invsqrt_max  = swift_cx8_recip_sqrt_fxp16(fs_max);

	uint32_t fs_term;
	uint32_t saturated = 0;

	/* denom > 0 requires fs_min < fs_max (=> invsqrt_min > invsqrt_max). Guard misconfig. */
	if (unlikely(invsqrt_min <= invsqrt_max)) {
		fs_term = 0;
		saturated = 1;
	} else if (invsqrt_cwnd <= invsqrt_max) {
		fs_term = 0;            /* cwnd >= fs_max: no headroom */
		saturated = 1;
	} else if (invsqrt_cwnd >= invsqrt_min) {
		fs_term = fs_range;     /* cwnd <= fs_min: max headroom */
		saturated = 1;
	} else {
		uint32_t num   = invsqrt_cwnd - invsqrt_max;
		uint32_t denom = invsqrt_min - invsqrt_max;
		fs_term = (uint32_t)(((uint64_t)fs_range * num) / denom);
	}

	if (saturated && counter != NULL)
		counter[SWIFT_CX8_C_FLOW_SCALE_SAT]++;

	return base + hop_term + fs_term;
}

/*
 * Pacing denominator for the cwnd->rate mapping (ns): the last usable fabric RTT
 * sample, clamped to [min_rtt, SWIFT_CX8_PACING_RTT_MAX_NS].
 *
 * This is the paper's pacing law rate = cwnd/rtt (Algorithm 1 lines 29-32): as the
 * fabric RTT inflates the same cwnd maps to a lower rate, holding
 * inflight ~= rate * rtt = cwnd (Little's law) -- the self-adjusting property of a
 * real window, which a constant denominator loses (it admits
 * inflight ~= cwnd * rtt_live/min_rtt under load; see the README for the measured
 * A/B against the constant-min_rtt mapping that settled this).
 *
 * The min_rtt floor: min_rtt is by definition the no-load RTT, so a smaller sample is
 * measurement noise, not headroom. The 100 ms cap only guards the uint64 arithmetic in
 * swift_cx8_compute_bdp_fxp16 (see SWIFT_CX8_PACING_RTT_MAX_NS); no real sample gets near it.
 * No sample yet (== 0, zero-filled ctxt) also falls back to min_rtt.
 */
static inline uint32_t swift_cx8_pacing_rtt_ns(uint32_t *param, const cc_ctxt_swift_cx8_t *ccctx)
{
	uint32_t min_rtt = param[SWIFT_CX8_P_MIN_RTT_NS];
	uint32_t rtt = ccctx->last_fabric_rtt;

	if (rtt <= min_rtt)
		return min_rtt;
	if (unlikely(rtt > SWIFT_CX8_PACING_RTT_MAX_NS))
		return SWIFT_CX8_PACING_RTT_MAX_NS;
	return rtt;
}

/*
 * Map cwnd (FXP16 packets) to the FXP20 rate fraction via the BDP identity at the given
 * RTT (the pacing denominator, see swift_cx8_pacing_rtt_ns):
 *   BDP_pkts   = line_rate_mbps * rtt_ns / (mss_bytes * 8000)   [since 1e6*1e-9=1e-3]
 *   bdp_fxp16  = line_rate_mbps * rtt_ns * 2^16 / (mss_bytes * 8000)
 *   rate_fxp20 = cwnd_fxp16 * 2^20 / bdp_fxp16
 * cwnd==BDP -> MAX_RATE; cwnd<1 -> deep sub-line pacing (Algorithm 1 pacing regime).
 */
static inline uint32_t swift_cx8_compute_bdp_fxp16(uint32_t *param, uint32_t rtt_ns)
{
	uint64_t num = (uint64_t)param[SWIFT_CX8_P_LINE_RATE_MBPS] * rtt_ns * (1ull << 16);
	uint64_t den = (uint64_t)param[SWIFT_CX8_P_MSS_BYTES] * 8000ull;
	if (unlikely(den == 0))
		return 0;
	return (uint32_t)(num / den);
}

/*
 * Clamp the actuated rate to [min_rate, MAX_RATE], reporting which bound (if any) hit.
 */
static inline uint32_t swift_cx8_clamp_rate(uint32_t rate, uint32_t min_rate,
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
 * Convert cwnd -> clamped FXP20 rate at the given pacing RTT, recording clamp counters.
 */
static inline uint32_t swift_cx8_cwnd_to_rate(uint32_t *param, uint32_t *counter, uint32_t cwnd_fxp16,
					  uint32_t rtt_ns)
{
	uint32_t bdp_fxp16 = swift_cx8_compute_bdp_fxp16(param, rtt_ns);
	uint32_t rate;
	uint32_t clamp_min = 0, clamp_max = 0;

	if (unlikely(bdp_fxp16 == 0))
		rate = DOCA_PCC_DEV_MAX_RATE; /* misconfigured params; fail open to line rate */
	else
		rate = (uint32_t)(((uint64_t)cwnd_fxp16 << 20) / bdp_fxp16);

	rate = swift_cx8_clamp_rate(rate, param[SWIFT_CX8_P_MIN_RATE_FXP20], &clamp_min, &clamp_max);
	if (counter != NULL) {
		if (clamp_min)
			counter[SWIFT_CX8_C_RATE_CLAMPED_MIN]++;
		if (clamp_max)
			counter[SWIFT_CX8_C_RATE_CLAMPED_MAX]++;
	}
	return rate;
}

/*
 * Actuate fcwnd AS A WINDOW. Every handler calls this as its last step, so every exit
 * path leaves results fully written.
 *
 * cwnd >= 1 packet: the window is expressed in credits and the rate limiter is opened
 *   to MAX_RATE. This is the paper's model -- transmission is ACK-clocked and the window
 *   alone bounds the bytes in flight, so nothing needs a rate computation at all.
 * cwnd <  1 packet: a credit window cannot express a fraction of a packet. The paper
 *   handles exactly this case by switching to pacing (Algorithm 1 lines 29-32:
 *   pacing_delay = rtt/cwnd), so we floor the window at one packet and let the FXP20
 *   rate limiter space that packet out -- which is what swift_cx8_cwnd_to_rate computes,
 *   dividing by the last measured fabric RTT.
 *
 * FIXME (assumption 1 -- credit UNIT). results->credits is assumed to count whole
 * packets, matching cwnd's unit. It could equally be bytes or MTUs; doca_pcc_dev.h:75-76
 * documents the field with a single line and no unit, there is no getter/setter helper,
 * no reference application sets it, and the BlueField-3 little-endian result struct has
 * no backing field to inspect. If the unit turns out to be bytes, multiply by
 * param[SWIFT_CX8_P_MSS_BYTES] here and nowhere else.
 *
 * FIXME (assumption 2 -- reload_credits SEMANTICS). Set equal to credits, i.e. read as
 * "the value the credit bucket refills to". The rival reading is an edge-triggered
 * "reload now" command, which would be 0 or 1 rather than a quantity. Nothing
 * distinguishes them: neither field appears in any DOCA documentation beyond the header
 * one-liners, in any public code, or in any NVIDIA sample. Unresolvable without silicon
 * that honours the fields.
 *
 * NB: rtt_req is deliberately NOT set here. The probe state machine owns it, and each
 * handler sets it for itself.
 */
static inline void swift_cx8_actuate(uint32_t *param, uint32_t *counter,
				     cc_ctxt_swift_cx8_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	uint32_t cwnd = ccctx->fcwnd; /* FXP16 packets */
	uint32_t rate, credits;

	if (cwnd >= SWIFT_CX8_CWND_ONE_FXP16) {
		credits = (cwnd + (1u << 15)) >> 16; /* round to whole packets */
		rate = DOCA_PCC_DEV_MAX_RATE;        /* window governs, not the limiter */
	} else {
		credits = 1; /* one packet, spaced out by the rate below */
		rate = swift_cx8_cwnd_to_rate(param, counter, cwnd,
					      swift_cx8_pacing_rtt_ns(param, ccctx));
	}

	ccctx->cur_rate = rate;
	results->rate = rate;
	results->credits = credits;
	results->reload_credits = credits;
}

/* Saturating 64->32 narrowing add, used by the additive-increase paths. The max_cwnd
 * clamp that follows every call makes the saturation bound itself uncritical; it exists
 * so a pathological num_acked cannot wrap cwnd to a small value. */
static inline uint32_t swift_cx8_sat_add32(uint32_t base, uint64_t add)
{
	uint64_t sum = (uint64_t)base + add;

	if (unlikely(sum > UINT32_MAX))
		return UINT32_MAX;
	return (uint32_t)sum;
}

/*
 * Compute target/delay in FXP16 for the MD branch, clamped to (0, FXP16_ONE).
 * Mirrors TIMELY's t_high_over_rtt idiom. Only called when delay >= target, so the
 * ratio is in (0, 1]; we return FXP16_ONE when delay <= target (no decrease).
 */
static inline uint32_t swift_cx8_target_over_delay_fxp16(uint32_t target, uint32_t delay)
{
	if (delay == 0 || delay <= target)
		return SWIFT_CX8_FXP16_ONE;
	uint64_t ratio = ((uint64_t)target << 16) / delay;
	if (ratio >= SWIFT_CX8_FXP16_ONE)
		ratio = SWIFT_CX8_FXP16_ONE - 1;
	return (uint32_t)ratio;
}

static inline uint32_t swift_cx8_clamp_cwnd(uint32_t cwnd_fxp16, uint32_t min_cwnd, uint32_t max_cwnd)
{
	if (cwnd_fxp16 < min_cwnd)
		cwnd_fxp16 = min_cwnd;
	if (cwnd_fxp16 > max_cwnd)
		cwnd_fxp16 = max_cwnd;
	return cwnd_fxp16;
}

/*
 * Handle new flow initialization.
 * Start cwnd at init_cwnd (~BDP), actuate it as a credit window, and request the first
 * RTT probe so the ACK handler has a delay sample to regulate on. Paper §3.2: the
 * initial cwnd has little effect.
 */
static inline void swift_cx8_handle_new_flow(doca_pcc_dev_event_t *event,
					 uint32_t *param,
					 uint32_t *counter,
					 cc_ctxt_swift_cx8_t *ccctx,
					 doca_pcc_dev_results_t *results)
{
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	uint32_t init_cwnd = param[SWIFT_CX8_P_INIT_CWND];

	ccctx->fcwnd = init_cwnd;
	ccctx->ecwnd = init_cwnd;          /* reserved (phase 2) */
	ccctx->endpoint_delay_ewma = 0;    /* reserved (phase 2) */
	ccctx->t_last_decrease = timestamp;
	ccctx->probe_start_ts = timestamp;
	ccctx->retx_cnt = 0;
	ccctx->probe_in_flight = 0;
	ccctx->abort_cnt = 0;
	ccctx->tx_since_probe_req = 1;
	ccctx->last_fabric_rtt = 0;        /* no sample yet -> AIMD stays parked until one lands */
	ccctx->last_ack_psn = 0;
	ccctx->flags = 0;                  /* PSN_VALID clear: first ACK seeds the tracker */

	swift_cx8_actuate(param, counter, ccctx, results);
	results->rtt_req = 1; /* request first RTT probe */

	SWIFT_CX8_TRACE(param, SWIFT_CX8_TRACE_FMT_NEW_FLOW,
		    SWIFT_CX8_TRACE_TSFLOW(timestamp, doca_pcc_dev_get_flowtag(event)), init_cwnd,
		    param[SWIFT_CX8_P_LINE_RATE_MBPS],
		    swift_cx8_compute_bdp_fxp16(param, param[SWIFT_CX8_P_MIN_RTT_NS]),
		    ccctx->cur_rate);
	SWIFT_CX8_TRACE_FLUSH(param);
}

/*
 * Handle TX event: maintain the RTT-probe abort/retry state machine (ported verbatim
 * from rtt_template / TIMELY). Swift does not change cwnd on TX. Without this machine a
 * single dropped RTT response stalls the flow forever.
 *
 * DOCA mapping: the paper assumes the NIC produces RTT measurements directly from ACK
 * timestamps; here we explicitly request probes via results->rtt_req. Model mapping, not
 * a paper deviation.
 */
static inline void swift_cx8_handle_tx(doca_pcc_dev_event_t *event,
				   uint32_t *param,
				   cc_ctxt_swift_cx8_t *ccctx,
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
		if (unlikely((probe_out_time_ns > ((uint32_t)SWIFT_CX8_ABORT_TIME_NS << ccctx->abort_cnt)) ||
			     (ccctx->tx_since_probe_req > 2))) {
			rtt_req = 1;
			if (probe_out_time_ns > ((uint32_t)SWIFT_CX8_ABORT_TIME_NS << ccctx->abort_cnt))
				ccctx->abort_cnt += 1;
			ccctx->tx_since_probe_req = 1;
		}
	}

	/* Pass-through actuation: cwnd is unchanged, but every event must still write a
	 * complete results struct. counter == NULL on purpose -- the rate-clamp counters
	 * describe control decisions, and TX events are far too frequent to let them
	 * dominate the counter. */
	swift_cx8_actuate(param, NULL, ccctx, results);
	results->rtt_req = rtt_req;
}

/*
 * Handle RTT event = REFRESH THE DELAY SAMPLE, then re-arm the probe.
 *
 * Unlike device/rp/swift/, this variant does NOT run Algorithm 1 here. The paper's
 * control loop is per-ACK ("On Receiving ACK", Algorithm 1 lines 4-28) and that is where
 * it lives (swift_cx8_handle_ack); a probe response is a measurement, not a control
 * event. So this handler only extracts the fabric delay, stores it as the sample the ACK
 * handler regulates on, and asks for the next probe. cwnd is untouched here, and no
 * format 16/17 decision lines are emitted.
 *
 * delay = fabric_rtt, where:
 *   total_rtt     = arrival_ts - req_send_ts        (local-clock duration, t6 - t1)
 *   np_turnaround = resp_send_ts - req_recv_ts       (remote-clock duration, t4 - t2)
 *   fabric_rtt    = total_rtt - np_turnaround        (NIC-to-NIC, queue-aware)
 * Both terms are durations (each within one clock), so this is sync-free. np_turnaround
 * is the paper's "remote-queuing" (§3.1 Measuring Delays), reflected by the responder
 * and subtracted by the sender (§3.3). A response that does not carry it is dropped --
 * the paper defines no fallback and neither do we (see the drop path below). NO
 * serialization is subtracted: per the design decision (and matching TIMELY) it is
 * folded into base_target. The residual local NIC-Rx delay (paper t6 - t5; BF3 exposes
 * no t5) stays in fabric_rtt -- endpoint signal, deferred to the ecwnd window (phase 2).
 */
static inline void swift_cx8_handle_rtt(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    uint32_t *counter,
				    cc_ctxt_swift_cx8_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	uint32_t probe_in_flight = ccctx->probe_in_flight;

	/* Drop spurious RTT events not matching a probe we are waiting on (e.g. from an
	 * aborted probe). Mirrors rtt_template / TIMELY. */
	if (unlikely((probe_in_flight == 0) && (ccctx->tx_since_probe_req == 0))) {
		swift_cx8_actuate(param, NULL, ccctx, results);
		results->rtt_req = 0;
		return;
	}
	ccctx->probe_in_flight = 0;
	ccctx->abort_cnt = 0;

	/* --- delay signal --- */
	uint32_t req_send  = doca_pcc_dev_get_rtt_req_send_timestamp(event);  /* t1 */
	uint32_t req_recv  = doca_pcc_dev_get_rtt_req_recv_timestamp(event);  /* t2 */
	uint32_t resp_send = doca_pcc_dev_get_rtt_resp_send_timestamp(event); /* t4 */
	uint32_t arrival   = doca_pcc_dev_get_timestamp(event);              /* t6 */

	uint32_t total_rtt = swift_cx8_diff_with_wrap32(arrival, req_send);
	uint32_t np_turnaround = 0;

	/* Packed tsflow (ts half = arrival/t6) for the format-19 line this handler emits --
	 * see SWIFT_CX8_TRACE_TSFLOW. Computed ahead of the drop-path check so the line
	 * carries it even for a dropped sample. The decision lines (16/17) come from the ACK
	 * handler now and carry their own tsflow. */
	uint64_t tsflow = SWIFT_CX8_TRACE_TSFLOW(arrival, doca_pcc_dev_get_flowtag(event));

	/* Raw probe timestamps (format 19): "did the responder stamp t2/t4?". Emitted for
	 * dropped samples too -- it is the diagnostic that explains the drops. populated:
	 * bit0 = t2 nonzero, bit1 = t4 nonzero; 3 = usable sample. t6/arrival is folded into
	 * tsflow instead of passed as its own field. */
	SWIFT_CX8_TRACE(param, SWIFT_CX8_TRACE_FMT_RTT_TS, tsflow, req_send, req_recv, resp_send,
		    (uint64_t)(req_recv != 0) | ((uint64_t)(resp_send != 0) << 1));

	if (resp_send != 0 && req_recv != 0)
		np_turnaround = swift_cx8_diff_with_wrap32(resp_send, req_recv);

	/* Paper §3.1 (Measuring Delays): the responder reflects its Rx->Tx time (t4 - t2,
	 * "remote-queuing") on every ACK and the sender subtracts it from the end-to-end
	 * RTT; the paper defines NO fallback for endpoints that do not report it. An
	 * unstamped (t2/t4 zero) or physically inconsistent (turnaround >= total RTT)
	 * response is therefore not a delay sample: drop it, count it, keep probing. cwnd
	 * stays untouched, and if no sample is EVER usable the ACK handler keeps parking on
	 * last_fabric_rtt == 0, so a non-stamping responder (e.g. a ConnectX-5 reflecting
	 * probes without timestamps) leaves Swift visibly inert -- AI/MD counters frozen,
	 * RTT_DROPPED_UNSTAMPED climbing -- instead of silently regulating on total RTT. */
	if (unlikely(resp_send == 0 || req_recv == 0 || np_turnaround >= total_rtt)) {
		if (counter != NULL)
			counter[SWIFT_CX8_C_RTT_DROPPED_UNSTAMPED]++;
		/* Fully inert otherwise: not a delay sample, and not loss-counter
		 * progress either -- data ACKs own the retx_cnt reset (Algorithm 1
		 * line 5, swift_cx8_handle_ack). Just re-arm the next probe. */
		swift_cx8_actuate(param, counter, ccctx, results);
		results->rtt_req = 1;
		ccctx->tx_since_probe_req = 1;
		SWIFT_CX8_TRACE_FLUSH(param);
		return;
	}

	uint32_t fabric_rtt = total_rtt - np_turnaround; /* paper §3.3: RTT minus the endpoint term */

	/* Usable sample: this is the delay the ACK handler compares against the target
	 * until the next probe lands, and the pacing denominator once cwnd drops below one
	 * packet. The drop path above never reaches here, so an unstamped responder leaves
	 * the last good sample (or "no sample yet") in place. */
	ccctx->last_fabric_rtt = fabric_rtt;

	/* === Algorithm 1 line 5: "On Receiving ACK: retransmit_cnt <- 0" ===
	 * Data ACKs reset this in swift_cx8_handle_ack; a usable RTT measurement is
	 * equally forward progress, so it resets here too. */
	ccctx->retx_cnt = 0;

	/* No window change: actuate the window we already have, and re-arm the probe. */
	swift_cx8_actuate(param, counter, ccctx, results);
	results->rtt_req = 1; /* request next RTT probe */
	ccctx->tx_since_probe_req = 1;

	SWIFT_CX8_TRACE_FLUSH(param);
}

/*
 * Handle NACK event = the loss / retransmit signal (paper §3.6, Algorithm 1 "On
 * Retransmit Timeout"). retx_cnt++; on reaching the threshold, collapse cwnd to
 * min_cwnd; otherwise apply the maximum multiplicative cut (1 - max_mdf), once per RTT.
 *
 * FIXME: every NACK is folded into the conservative RTO path (retx_cnt++ toward the
 * collapse threshold). MEASURED 2026-07-06: ev_subtype IS populated -- ramp NACKs
 * arrive as OOS (=2; doca_pcc_dev_nack_event_sub_type_enum: 1=RNR, 2=OOS,
 * 3=DUP_READ) -- so the paper's split is implementable: OOS/DUP_READ are
 * SACK/fast-recovery-class (Algorithm 1 lines 22-25: retx_cnt<-0, single MD), while
 * RNR is receiver-app backpressure, arguably not congestion at all. The enum has NO
 * timeout subtype: a true RTO retransmits from firmware without a received NACK
 * packet, so it may never surface here -- which reframes what the retx_reset
 * threshold actually guards. Unimplemented pending a design decision. Data ACKs
 * (swift_cx8_handle_ack) and usable RTT samples reset retx_cnt, so isolated NACKs decay
 * rather than accumulate to a reset.
 */
static inline void swift_cx8_handle_nack(doca_pcc_dev_event_t *event,
				     uint32_t *param,
				     uint32_t *counter,
				     cc_ctxt_swift_cx8_t *ccctx,
				     doca_pcc_dev_results_t *results)
{
	uint32_t now = doca_pcc_dev_get_timestamp(event);
	uint32_t min_cwnd = param[SWIFT_CX8_P_MIN_CWND];
	uint32_t max_cwnd = param[SWIFT_CX8_P_MAX_CWND];
	uint32_t max_mdf  = param[SWIFT_CX8_P_MAX_MDF_FXP16];
	uint32_t cwnd = ccctx->fcwnd;
	uint32_t reset = 0;

	uint32_t retx_cnt = ccctx->retx_cnt + 1;
	ccctx->retx_cnt = retx_cnt;

	/* No measured RTT on a NACK; gate the MD with min_rtt as the once-per-RTT period. */
	uint32_t since_dec = swift_cx8_diff_with_wrap32(now, ccctx->t_last_decrease);
	uint32_t can_decrease = (since_dec >= param[SWIFT_CX8_P_MIN_RTT_NS]);

	if (retx_cnt >= param[SWIFT_CX8_P_RETX_RESET_THRESH]) {
		cwnd = min_cwnd;
		reset = 1;
		ccctx->t_last_decrease = now;
		if (counter != NULL)
			counter[SWIFT_CX8_C_NACK_RESET]++;
	} else if (can_decrease) {
		uint32_t factor = SWIFT_CX8_FXP16_ONE - max_mdf; /* 1 - max_mdf */
		cwnd = doca_pcc_dev_fxp_mult(factor, cwnd);
		ccctx->t_last_decrease = now;
		if (counter != NULL)
			counter[SWIFT_CX8_C_NACK_MD]++;
	}

	cwnd = swift_cx8_clamp_cwnd(cwnd, min_cwnd, max_cwnd);
	ccctx->fcwnd = cwnd;

	swift_cx8_actuate(param, counter, ccctx, results);
	results->rtt_req = 0;

	/* Always emitted (not gated by trace_on): NACKs are rare (~2 per run at ramp on
	 * this fabric), a loss event is always worth a line, and ev_subtype is being
	 * surveyed to learn whether firmware distinguishes NACK causes (see FIXME above --
	 * it decides whether the fast-recovery/RTO split is implementable). retx_reset packs
	 * retx_cnt into bits[31:1] and the reset flag into bit0 (see SWIFT_CX8_TRACE_TSFLOW /
	 * README) to fit tsflow into the same five doca_pcc_dev_trace_5 arg slots. */
	doca_pcc_dev_trace_5(SWIFT_CX8_TRACE_FMT_LOSS,
			     SWIFT_CX8_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)),
			     doca_pcc_dev_get_ev_attr(event).ev_subtype,
			     ((uint64_t)(retx_cnt) << 1) | (reset & 1), cwnd, ccctx->cur_rate);
	doca_pcc_dev_trace_flush();
}

/*
 * num_acked (Algorithm 1's argument to the AI step): how many packets this ACK event
 * acknowledged.
 *
 * The RoCE ACK carries a PSN, so the natural estimate is the wrap-safe difference from
 * the previous ACK's PSN. Two things can spoil that: the very first ACK on a flow has no
 * predecessor, and a reordered or stale PSN can produce an absurd delta. Both fall back
 * to the event's own num_coalesced count, and the result is capped at roughly two windows
 * (floor 256) so a bad sample cannot inject an enormous increase.
 *
 * Ported from smartt_reps_psn_delta (device/rp/smartt_reps/algo/smartt_reps.c), minus its
 * anomaly counter -- swift_cx8 keeps the swift counter table unchanged.
 */
static inline uint32_t swift_cx8_psn_delta(cc_ctxt_swift_cx8_t *ccctx, uint32_t psn, uint32_t ncoal)
{
	uint32_t delta;
	uint32_t cwnd_pkts = ccctx->fcwnd >> 16;
	uint32_t cap = 2u * cwnd_pkts + 2u;

	if (cap < 256u)
		cap = 256u;

	if (ccctx->flags & SWIFT_CX8_FLAG_PSN_VALID) {
		delta = swift_cx8_diff_with_wrap32(psn, ccctx->last_ack_psn);
		if (unlikely(delta > cap))
			delta = ncoal;
	} else {
		delta = ncoal;
	}
	if (unlikely(delta > cap))
		delta = cap;

	ccctx->last_ack_psn = psn;
	ccctx->flags |= SWIFT_CX8_FLAG_PSN_VALID;
	return delta;
}

/*
 * Handle ACK event -- THE MAIN HANDLER (paper Algorithm 1 "On Receiving ACK", lines
 * 4-28, with the flow-scaled target of §3.5).
 *
 * This is the placement the paper describes and the main gain of swift_cx8 over
 * device/rp/swift/, which runs the same control law once per RTT-probe event instead.
 * ACKs arrive continuously (roughly 5-packet coalescing at line rate on this fabric), so
 * the window now moves at ACK granularity and the additive increase is spread across the
 * window exactly as Algorithm 1 spreads it: each ACK contributes (ai/cwnd)*num_acked, so
 * a full window of ACKs adds ai in total.
 *
 * The delay it regulates on is still the last RTT-probe sample, because BlueField-3
 * gives no per-ACK delay. That is the one signal-side deviation that survives here.
 */
static inline void swift_cx8_handle_ack(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    uint32_t *counter,
				    cc_ctxt_swift_cx8_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	/* === Algorithm 1 line 5: retransmit_cnt <- 0 === */
	ccctx->retx_cnt = 0;

	uint32_t now = doca_pcc_dev_get_timestamp(event);
	uint32_t psn = doca_pcc_dev_get_roce_ack_first_sn(event);
	doca_pcc_dev_ack_nack_cnp_extra_t extra = doca_pcc_dev_get_ack_nack_cnp_extra(event);
	uint32_t ncoal = extra.num_coalesced;

	if (ncoal == 0)
		ncoal = 1; /* treat 0 as 1 */

	uint32_t num_acked = swift_cx8_psn_delta(ccctx, psn, ncoal);
	uint32_t delay = ccctx->last_fabric_rtt;

	/* No usable probe sample yet (or every sample so far was dropped for missing
	 * responder stamps): there is nothing to compare against the target, so do not
	 * regulate. Holding the window is the honest response to a missing signal -- the
	 * alternative, treating "unknown" as "uncongested", would ramp blindly. */
	if (unlikely(delay == 0)) {
		swift_cx8_actuate(param, counter, ccctx, results);
		results->rtt_req = 0;
		return;
	}

	uint32_t ai       = param[SWIFT_CX8_P_AI_FXP16];
	uint32_t md_beta  = param[SWIFT_CX8_P_MD_BETA_FXP16];
	uint32_t max_mdf  = param[SWIFT_CX8_P_MAX_MDF_FXP16];
	uint32_t min_cwnd = param[SWIFT_CX8_P_MIN_CWND];
	uint32_t max_cwnd = param[SWIFT_CX8_P_MAX_CWND];

	/* === Algorithm 1 line 2: cwnd_prev <- cwnd === */
	uint32_t cwnd = ccctx->fcwnd;
	uint32_t cwnd_prev = cwnd;

	/* === Algorithm 1 line 3: can_decrease = (now - t_last_decrease >= rtt) ===
	 * Unlike the probe-driven variant, this guard is now load-bearing: many ACKs arrive
	 * within one RTT and without it every one of them would cut the window, turning a
	 * single congestion signal into a collapse. The period is the fabric RTT (the only
	 * RTT this handler has); it is slightly shorter than the full round trip, which
	 * makes the guard marginally permissive rather than marginally strict. */
	uint32_t since_dec = swift_cx8_diff_with_wrap32(now, ccctx->t_last_decrease);
	uint32_t can_decrease = (since_dec >= delay);

	/* === Algorithm 1 line 6: target <- TargetDelay() (uses current cwnd) === */
	uint32_t target = swift_cx8_flow_scaled_target(param, cwnd, counter);

	uint32_t branch;

	if (delay < target) {
		/* === Algorithm 1 lines 7-11: Additive Increase === */
		if (counter != NULL)
			counter[SWIFT_CX8_C_DELAY_LT_TARGET]++;
		if (cwnd >= SWIFT_CX8_CWND_ONE_FXP16) {
			/* cwnd += (ai/cwnd) * num_acked. ai/cwnd is a ratio of two FXP16
			 * quantities, so it needs a re-scale: (ai << 16) / cwnd puts it back
			 * in FXP16 packets. Spreading ai across the window this way is what
			 * makes one window of ACKs add exactly ai. */
			uint64_t ai_over_cwnd = ((uint64_t)ai << 16) / cwnd;

			cwnd = swift_cx8_sat_add32(cwnd, ai_over_cwnd * num_acked);
		} else {
			/* Below one packet the window is not spread over multiple packets, so
			 * the paper adds the full ai per acknowledged packet (Algorithm 1
			 * line 10). */
			cwnd = swift_cx8_sat_add32(cwnd, (uint64_t)ai * num_acked);
		}
		branch = SWIFT_CX8_BRANCH_AI;
		if (counter != NULL)
			counter[SWIFT_CX8_C_AI]++;
	} else {
		/* === Algorithm 1 lines 12-14: Multiplicative Decrease (once per RTT) === */
		if (counter != NULL)
			counter[SWIFT_CX8_C_DELAY_GT_TARGET]++;
		if (can_decrease) {
			uint32_t ratio = swift_cx8_target_over_delay_fxp16(target, delay); /* target/delay */
			uint32_t frac  = SWIFT_CX8_FXP16_ONE - ratio;                      /* (delay-target)/delay */
			uint32_t beta_term = doca_pcc_dev_fxp_mult(md_beta, frac);
			if (beta_term >= SWIFT_CX8_FXP16_ONE)
				beta_term = SWIFT_CX8_FXP16_ONE - 1;
			uint32_t factor = SWIFT_CX8_FXP16_ONE - beta_term;
			uint32_t floor = SWIFT_CX8_FXP16_ONE - max_mdf;                    /* 1 - max_mdf */
			if (factor < floor)
				factor = floor;
			cwnd = doca_pcc_dev_fxp_mult(factor, cwnd);
			branch = SWIFT_CX8_BRANCH_MD;
			if (counter != NULL)
				counter[SWIFT_CX8_C_MD]++;
		} else {
			branch = SWIFT_CX8_BRANCH_MD_BLOCKED;
			if (counter != NULL)
				counter[SWIFT_CX8_C_CAN_DECREASE_BLOCK]++;
		}
	}

	/* === Algorithm 1 line 26: clamp(min_cwnd, cwnd, max_cwnd) ===
	 * The clamp bit reports the WINDOW hitting a bound, not the rate: with the limiter
	 * pinned open above one packet, min_cwnd/max_cwnd are the only guards left. */
	uint32_t cwnd_unclamped = cwnd;

	cwnd = swift_cx8_clamp_cwnd(cwnd, min_cwnd, max_cwnd);
	if (cwnd != cwnd_unclamped)
		branch |= SWIFT_CX8_BRANCH_CLAMPED_BIT;

	/* === Algorithm 1 lines 27-28: if cwnd <= cwnd_prev then t_last_decrease <- now === */
	if (cwnd <= cwnd_prev)
		ccctx->t_last_decrease = now;

	if (cwnd < SWIFT_CX8_CWND_ONE_FXP16) {
		branch |= SWIFT_CX8_BRANCH_CWND_LT1_BIT;
		if (counter != NULL)
			counter[SWIFT_CX8_C_CWND_BELOW_ONE]++;
	}

	ccctx->fcwnd = cwnd;

	/* === Algorithm 1 lines 29-32 + Output === */
	swift_cx8_actuate(param, counter, ccctx, results);
	results->rtt_req = 0;

	/* Trace volume: an AI line per ACK would melt the tracer at line rate (ACKs coalesce
	 * only ~5 packets), so the steady-state increase is sampled 1-in-64 by PSN. Decrease
	 * decisions are rare and individually interesting, so they are never sampled out.
	 *
	 * Field remap vs device/rp/swift/: format 16's third field carries num_acked here,
	 * not np_turnaround (no probe decomposition happens on an ACK). The host prints it
	 * under the label np_turn= -- see the trace table in README.md. */
	if ((branch & 0x0F) != SWIFT_CX8_BRANCH_AI || (psn & 63) == 0) {
		uint64_t tsflow = SWIFT_CX8_TRACE_ENABLED(param) ?
				  SWIFT_CX8_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)) : 0;

		SWIFT_CX8_TRACE(param, SWIFT_CX8_TRACE_FMT_RTT_DELAY, tsflow, delay, num_acked,
			    target, branch);
		SWIFT_CX8_TRACE(param, SWIFT_CX8_TRACE_FMT_RTT_WINDOW, tsflow, cwnd,
			    target - param[SWIFT_CX8_P_BASE_TARGET_NS], can_decrease,
			    ccctx->cur_rate);
		SWIFT_CX8_TRACE_FLUSH(param);
	}
}

/*
 * Handle CNP event. Swift is delay-based and does not react to ECN (paper §3.7 isolates
 * traffic via QoS, not CNP). Re-actuate the unchanged window; the next ACK adjusts cwnd
 * from the measured delay.
 */
static inline void swift_cx8_handle_cnp(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    cc_ctxt_swift_cx8_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	swift_cx8_actuate(param, NULL, ccctx, results);
	results->rtt_req = 0;
}

void swift_cx8_algo(doca_pcc_dev_event_t *event,
		uint32_t *param,
		uint32_t *counter,
		doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		doca_pcc_dev_results_t *results)
{
	cc_ctxt_swift_cx8_t *swift_cx8_ctx = (cc_ctxt_swift_cx8_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = swift_cx8_ctx->cur_rate;

	if (counter != NULL)
		counter[SWIFT_CX8_C_EVENTS]++;

	if (unlikely(cur_rate == 0)) {
		swift_cx8_handle_new_flow(event, param, counter, swift_cx8_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_RTT) {
		swift_cx8_handle_rtt(event, param, counter, swift_cx8_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_ACK) {
		swift_cx8_handle_ack(event, param, counter, swift_cx8_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		swift_cx8_handle_tx(event, param, swift_cx8_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		swift_cx8_handle_nack(event, param, counter, swift_cx8_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		swift_cx8_handle_cnp(event, param, swift_cx8_ctx, results);
	} else {
		swift_cx8_actuate(param, NULL, swift_cx8_ctx, results);
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t swift_cx8_set_algo_params(uint32_t param_id_base,
					   uint32_t param_num,
					   const uint32_t *new_param_values,
					   uint32_t *params)
{
	/* Reject writes that would run off the end of the param array. param_id_base
	 * and param_num are bounded individually AND together: param_id_base+param_num
	 * must stay within SWIFT_CX8_NUM_PARAMS, otherwise the copy below writes OOB. */
	if ((param_id_base >= SWIFT_CX8_NUM_PARAMS) ||
	    (param_num > SWIFT_CX8_NUM_PARAMS) ||
	    (param_id_base + param_num > SWIFT_CX8_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* Commit the validated values into the live param array. The framework does
	 * NOT auto-copy: per the doca_pcc_dev_user_set_algo_params contract, returning
	 * OK means "the callback set them" (FAIL means "nothing changed"). Without this
	 * loop, the array returned by doca_pcc_dev_get_algo_params() keeps the init-time
	 * defaults, so PPCC writes read back correctly (register image) yet never reach
	 * the running algorithm. Range is already enforced by the min/max registered in
	 * swift_cx8_init(); the fs_min_cwnd < fs_max_cwnd ordering invariant is additionally
	 * guarded on-device in swift_cx8_flow_scaled_target (denom <= 0 -> fs_term = 0), so
	 * a transient bad pair degrades gracefully rather than dividing by zero. */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
