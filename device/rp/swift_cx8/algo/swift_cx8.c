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
 * Swift-CX8: delay-based, window-AIMD congestion control, actuated as a credit window.
 * Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
 * Datacenter", SIGCOMM 2020 -- Algorithm 1 (§3.2) with the flow-scaled target (§3.5).
 *
 * READ THIS FIRST: this variant assumes doca_pcc_dev_results_t.credits /
 * .reload_credits reach the hardware. On every DOCA 2.10 chip they do NOT
 * (libdoca_pcc_dev.a's glue reads only rate and rtt_req; confirmed by SwCC, USENIX
 * ATC 2025, §3.4), and because this variant pins the rate limiter open whenever
 * cwnd >= 1 packet, it degenerates to an UNCONTROLLED SENDER on current silicon. It
 * is a paper-faithful reference, not a benchmark target -- for measurements use
 * device/rp/swift/. See README.md.
 *
 * vs device/rp/swift/: AIMD runs per ACK with the paper's (ai/cwnd)*num_acked
 * increase, and the window is actuated as a window (credits). The delay signal still
 * comes from explicit RTT probes; v1 is fabric-only (ecwnd reserved).
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

/* Trace emission, gated at runtime by the trace_on PPCC param (see swift_cx8.h). */
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
 * 32-bit wrap-safe timestamp difference. Local static copy: the ALWAYS_INLINE
 * diff_with_wrap32 in pcc_common_dev.h duplicate-symbol clashes under -flto when
 * pulled into two translation units.
 */
static inline uint32_t swift_cx8_diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	if (greater_num < smaller_num)
		return UINT32_MAX - smaller_num + greater_num + 1;
	return greater_num - smaller_num;
}

/* Contract descriptor strings. const volatile prevents LTO dead-code elimination:
 * they are read out-of-band by the host, never dereferenced on-device. */
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

	/* Register from the contract table. Passing the table's min (not a hardcoded 1)
	 * lets hop_scale and trace_on accept a write of 0. */
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
 * Reciprocal square root in FXP16: (1/sqrt(x)) * 2^16, via the identity
 * 1/sqrt(x) = 2^(-0.5*log2(x)) on the DPA's FXP16 log2/pow2 intrinsics (no float, no
 * sqrt on the DPA). Validated on silicon: within 0.13% of fs_range over 0.1-1024 pkt.
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
 * Flow-scaled target delay (paper §3.5):
 *   t = base_target + num_hops*hop_scale + max(0, min(alpha/sqrt(fcwnd) + beta, fs_range))
 * Since beta = -alpha*invsqrt(fs_max), the scaled term collapses to a linear
 * interpolation of fs_range over the 1/sqrt(cwnd) axis between the knees:
 *   fs_term = fs_range * (invsqrt(cwnd) - invsqrt(fs_max)) / (invsqrt(fs_min) - invsqrt(fs_max))
 * clamped to [0, fs_range].
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
 * Pacing denominator (ns): the last usable fabric RTT sample, clamped to
 * [min_rtt, SWIFT_CX8_PACING_RTT_MAX_NS]. Only consulted below one packet, where the
 * rate limiter implements the paper's pacing regime (Algorithm 1 lines 29-32).
 * No sample yet (zero-filled ctxt) falls back to min_rtt.
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
 * BDP in FXP16 packets at the given RTT:
 *   bdp_fxp16 = line_rate_mbps * rtt_ns * 2^16 / (mss_bytes * 8000)   [1e6*1e-9 = 1e-3]
 * The caller maps rate_fxp20 = cwnd_fxp16 * 2^20 / bdp_fxp16.
 */
static inline uint32_t swift_cx8_compute_bdp_fxp16(uint32_t *param, uint32_t rtt_ns)
{
	uint64_t num = (uint64_t)param[SWIFT_CX8_P_LINE_RATE_MBPS] * rtt_ns * (1ull << 16);
	uint64_t den = (uint64_t)param[SWIFT_CX8_P_MSS_BYTES] * 8000ull;
	if (unlikely(den == 0))
		return 0;
	return (uint32_t)(num / den);
}

/* Clamp the actuated rate to [min_rate, MAX_RATE], reporting which bound hit. */
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

/* Convert cwnd -> clamped FXP20 rate at the given pacing RTT, recording clamp counters. */
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
 * Actuate fcwnd AS A WINDOW. Every handler calls this last, so every exit path leaves
 * results fully written. cwnd >= 1 packet: express the window in credits and pin the
 * rate limiter open (ACK-clocked, the paper's model). cwnd < 1 packet: credits cannot
 * express a fraction, so floor at one packet and let the FXP20 rate implement the
 * paper's pacing_delay = rtt/cwnd regime.
 * FIXME: credits is assumed to count whole packets; the header documents no unit. If
 * it is bytes, multiply by mss_bytes here and nowhere else.
 * FIXME: reload_credits is read as "the value the bucket refills to" and set equal to
 * credits; nothing distinguishes this from an edge-triggered "reload now" command.
 * rtt_req is deliberately not set here -- the probe state machine owns it.
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

/* Saturating 64->32 add: a pathological num_acked must not wrap cwnd to a small value. */
static inline uint32_t swift_cx8_sat_add32(uint32_t base, uint64_t add)
{
	uint64_t sum = (uint64_t)base + add;

	if (unlikely(sum > UINT32_MAX))
		return UINT32_MAX;
	return (uint32_t)sum;
}

/*
 * target/delay in FXP16 for the MD branch, clamped to (0, FXP16_ONE); returns
 * FXP16_ONE when delay <= target (no decrease).
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

/* New flow: start cwnd at init_cwnd, actuate as a credit window, request the first probe. */
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
 * TX event: maintain the RTT-probe abort/retry state machine (ported from
 * rtt_template). Swift does not change cwnd on TX; without this machine a single
 * dropped RTT response stalls the flow forever.
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

	/* Pass-through actuation: cwnd unchanged, but every event must write a complete
	 * results struct. counter == NULL: TX events are too frequent for clamp counters. */
	swift_cx8_actuate(param, NULL, ccctx, results);
	results->rtt_req = rtt_req;
}

/*
 * RTT event: refresh the delay sample, then re-arm the probe. The control loop is
 * per-ACK (Algorithm 1 "On Receiving ACK") and lives in swift_cx8_handle_ack; a probe
 * response is a measurement, not a control event.
 *   total_rtt     = arrival_ts - req_send_ts       (local clock, t6 - t1)
 *   np_turnaround = resp_send_ts - req_recv_ts     (remote clock, t4 - t2)
 *   fabric_rtt    = total_rtt - np_turnaround      (sync-free: both are durations)
 * Serialization is folded into base_target; the residual local NIC-Rx delay (paper
 * t6 - t5; BF3 exposes no t5) stays in the signal.
 */
static inline void swift_cx8_handle_rtt(doca_pcc_dev_event_t *event,
				    uint32_t *param,
				    uint32_t *counter,
				    cc_ctxt_swift_cx8_t *ccctx,
				    doca_pcc_dev_results_t *results)
{
	uint32_t probe_in_flight = ccctx->probe_in_flight;

	/* Drop spurious RTT events not matching a probe we are waiting on. */
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

	uint64_t tsflow = SWIFT_CX8_TRACE_TSFLOW(arrival, doca_pcc_dev_get_flowtag(event));

	/* Raw probe timestamps (format 19), emitted for dropped samples too.
	 * populated: bit0 = t2 nonzero, bit1 = t4 nonzero; 3 = usable sample. */
	SWIFT_CX8_TRACE(param, SWIFT_CX8_TRACE_FMT_RTT_TS, tsflow, req_send, req_recv, resp_send,
		    (uint64_t)(req_recv != 0) | ((uint64_t)(resp_send != 0) << 1));

	if (resp_send != 0 && req_recv != 0)
		np_turnaround = swift_cx8_diff_with_wrap32(resp_send, req_recv);

	/* Paper §3.1 defines no fallback for endpoints that do not reflect t4 - t2. An
	 * unstamped or inconsistent (turnaround >= total RTT) response is not a delay
	 * sample: dropped, counted, cwnd untouched, next probe re-armed. With no usable
	 * sample the ACK handler stays parked on last_fabric_rtt == 0, so a non-stamping
	 * responder leaves Swift visibly inert. */
	if (unlikely(resp_send == 0 || req_recv == 0 || np_turnaround >= total_rtt)) {
		if (counter != NULL)
			counter[SWIFT_CX8_C_RTT_DROPPED_UNSTAMPED]++;
		swift_cx8_actuate(param, counter, ccctx, results);
		results->rtt_req = 1;
		ccctx->tx_since_probe_req = 1;
		SWIFT_CX8_TRACE_FLUSH(param);
		return;
	}

	uint32_t fabric_rtt = total_rtt - np_turnaround;

	/* The delay the ACK handler regulates on until the next probe lands, and the
	 * pacing denominator below one packet. */
	ccctx->last_fabric_rtt = fabric_rtt;

	/* Algorithm 1 line 5: a usable RTT sample is forward progress, like a data ACK. */
	ccctx->retx_cnt = 0;

	/* No window change: actuate the window we already have, and re-arm the probe. */
	swift_cx8_actuate(param, counter, ccctx, results);
	results->rtt_req = 1; /* request next RTT probe */
	ccctx->tx_since_probe_req = 1;

	SWIFT_CX8_TRACE_FLUSH(param);
}

/*
 * NACK event = the loss signal (Algorithm 1 "On Retransmit Timeout"): retx_cnt++;
 * at the threshold collapse cwnd to min_cwnd, otherwise apply the maximum
 * multiplicative cut (1 - max_mdf), once per RTT.
 * FIXME: every NACK folds into the conservative RTO path. ev_subtype is populated
 * (measured: ramp NACKs arrive as OOS), so the paper's fast-recovery/RTO split is
 * implementable; unimplemented pending a design decision.
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

	/* Always emitted (ungated): loss is rare and always worth a line. The third
	 * field packs retx_cnt into bits[31:1] and the reset flag into bit0. */
	doca_pcc_dev_trace_5(SWIFT_CX8_TRACE_FMT_LOSS,
			     SWIFT_CX8_TRACE_TSFLOW(now, doca_pcc_dev_get_flowtag(event)),
			     doca_pcc_dev_get_ev_attr(event).ev_subtype,
			     ((uint64_t)(retx_cnt) << 1) | (reset & 1), cwnd, ccctx->cur_rate);
	doca_pcc_dev_trace_flush();
}

/*
 * num_acked: wrap-safe PSN delta from the previous ACK. The first ACK on a flow and
 * absurd deltas (reordered/stale PSN) fall back to the event's num_coalesced; the
 * result is capped at ~two windows (floor 256) so a bad sample cannot inject an
 * enormous increase. Ported from smartt_reps_psn_delta.
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
 * ACK event -- the main handler (Algorithm 1 "On Receiving ACK", lines 4-28, with the
 * flow-scaled target §3.5). The per-ACK placement is the main gain over
 * device/rp/swift/: each ACK contributes (ai/cwnd)*num_acked, so a window of ACKs
 * adds ai in total. The delay it regulates on is still the last RTT-probe sample
 * (BF3 gives no per-ACK delay).
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

	/* No usable probe sample yet: nothing to compare against the target, so hold
	 * the window rather than ramp blindly. */
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
	 * Load-bearing here: many ACKs arrive within one RTT, and without the guard each
	 * would cut the window. The period is the fabric RTT (the only RTT available). */
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
			/* cwnd += (ai/cwnd) * num_acked; (ai << 16) / cwnd re-scales the
			 * FXP16/FXP16 ratio back to FXP16 packets. */
			uint64_t ai_over_cwnd = ((uint64_t)ai << 16) / cwnd;

			cwnd = swift_cx8_sat_add32(cwnd, ai_over_cwnd * num_acked);
		} else {
			/* Below one packet the paper adds the full ai per acked packet (line 10). */
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
	 * The clamp bit reports the WINDOW hitting a bound, not the rate. */
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

	/* Steady-state AI lines are sampled 1-in-64 by PSN (a line per ACK would melt the
	 * tracer); decreases are never sampled out. FIELD REMAP: format 16's third field
	 * carries num_acked here; the shared host format string labels it np_turn=. */
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

/* CNP event: Swift is delay-based and does not react to ECN; re-actuate the window. */
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
	/* Reject writes that would run off the end of the param array. */
	if ((param_id_base >= SWIFT_CX8_NUM_PARAMS) ||
	    (param_num > SWIFT_CX8_NUM_PARAMS) ||
	    (param_id_base + param_num > SWIFT_CX8_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	/* The framework does NOT auto-copy: returning OK means "the callback set them".
	 * Without this loop PPCC writes read back correctly yet never reach the running
	 * algorithm. Ranges are enforced by the min/max registered in swift_cx8_init(); a
	 * bad fs_min/fs_max pair is additionally guarded in swift_cx8_flow_scaled_target. */
	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
