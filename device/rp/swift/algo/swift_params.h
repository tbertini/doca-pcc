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
 */

#ifndef SWIFT_PARAMS_H_
#define SWIFT_PARAMS_H_

#include <stdint.h>

/*
 * Single source of truth for Swift's PPCC parameter and counter contract.
 *
 * Included by:
 *   - swift.c (DPA): swift_init() registers params/counters from swift_params[].
 *   - swift_params_dump.c (host): emits JSON for incast-bench's parameter encoder.
 *
 * Swift (Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control
 * in the Datacenter", SIGCOMM 2020, DOI 10.1145/3387514.3406591) is a window-based
 * AIMD controller against an absolute, flow-scaled target delay. We control a
 * congestion window (cwnd, in packets, fractional) per Algorithm 1, and actuate it
 * on BlueField-3 by mapping cwnd -> Tx rate via the BDP identity (see swift.c). BF3
 * has no usable credit/window actuator (doca_pcc_dev_results_t.credits is unbacked),
 * so rate is the actuator; cwnd remains the control variable.
 *
 * Wire encodings (consistent with the timely/dcqcn contracts in this repo):
 *   - time          -> nanoseconds (uint32)
 *   - cwnd          -> FXP16 packets (1<<16 = 1.0 packet; 0.001 pkt = 66)
 *   - ratios/gains  -> FXP16 (1<<16 = 1.0): md_beta, max_mdf, endpoint_ewma
 *   - rate          -> FXP20 (1<<20 = 100% line rate): min_rate
 * Off-host tooling (incast-bench) is responsible for any human-unit -> FXP
 * conversion (e.g. Mbps -> FXP20) using the configured line rate.
 *
 * Defaults are aligned with the compile-time #defines in swift_algo_params.h so that
 * a doca_pcc instance running with no param writes behaves identically to a build
 * with the contract bypassed. The orchestrator overrides these at runtime over the
 * PPCC channel (cmd_type=0xB) when sweeping the parameter space.
 *
 * The flow-scaling coefficients alpha/beta of paper Eq. (§3.5) are DERIVED on-device
 * from fs_range/fs_min_cwnd/fs_max_cwnd and are intentionally NOT exposed as params.
 */

/* Swift algorithm ID. Project convention: TIMELY=0xBFFD, DCQCN=0xBFFE,
 * rtt_template/template=0xBFFF. Swift claims the next-lower free slot. */
#define SWIFT_ALGO_ID  0xBFFC

/*
 * Parameter indices.
 *
 * APPEND-ONLY: external orchestrators (incast-bench) address parameters by index in
 * bulk-set, so never reorder or insert -- only append before TRACE_ON is reassigned.
 * TRACE_ON is kept LAST so indices 0..N-2 stay stable across versions.
 *
 * Grouping: [0..5] target-delay scaling (§3.5); [6..12] AIMD on cwnd (Algorithm 1);
 * [13..16] cwnd->rate actuation (pacing law); [17..18] endpoint window (paper §3.3,
 * phase 2, reserved+inert in v1 so the index map is stable when ecwnd lands);
 * [19] diagnostics.
 *
 * TRACE_ON is a diagnostics-only toggle, NOT a tuning knob: it gates runtime emission
 * of the format 15/16/17/18 device traces (see swift.h). It rides in the param array
 * so it can be flipped at runtime over the existing PPCC channel without rebuilding.
 */
enum swift_param_idx {
	/* --- target delay scaling (paper §3.5) --- */
	SWIFT_P_BASE_TARGET_NS     = 0,  /* base fabric target; absorbs serialization/propagation/fixed delay */
	SWIFT_P_FS_RANGE_NS        = 1,  /* fs_range: max additional target for small flows */
	SWIFT_P_FS_MIN_CWND        = 2,  /* fs_min_cwnd: low knee, cwnd<=this -> fs_term=fs_range (FXP16 pkts) */
	SWIFT_P_FS_MAX_CWND        = 3,  /* fs_max_cwnd: high knee, cwnd>=this -> fs_term=0 (FXP16 pkts) */
	SWIFT_P_NUM_HOPS           = 4,  /* fixed #hops (BF3 TTL unreadable); per-hop term = num_hops*hop_scale */
	SWIFT_P_HOP_SCALE_NS       = 5,  /* hbar: per-hop target add; 0 on BF3 */
	/* --- AIMD on cwnd (paper Algorithm 1) --- */
	SWIFT_P_AI_FXP16           = 6,  /* ai: additive increment per RTT event (FXP16 pkts) */
	SWIFT_P_MD_BETA_FXP16      = 7,  /* beta: MD gain on (delay-target)/delay */
	SWIFT_P_MAX_MDF_FXP16      = 8,  /* max_mdf: max multiplicative decrease per event / on loss */
	SWIFT_P_MIN_CWND           = 9,  /* min_cwnd floor (FXP16 pkts); paper §3.4 = 0.001 pkt = 66 */
	SWIFT_P_MAX_CWND           = 10, /* max_cwnd ceiling (FXP16 pkts), near/above BDP */
	SWIFT_P_INIT_CWND          = 11, /* new-flow starting window (FXP16 pkts) */
	SWIFT_P_RETX_RESET_THRESH  = 12, /* consecutive NACKs -> cwnd=min_cwnd (paper §3.6) */
	/* --- cwnd -> rate actuation (BDP identity) --- */
	SWIFT_P_MSS_BYTES          = 13, /* segment size for BDP (bytes) */
	SWIFT_P_MIN_RTT_NS         = 14, /* base RTT for BDP + can_decrease/pacing reference (ns) */
	SWIFT_P_LINE_RATE_MBPS     = 15, /* NIC line rate for BDP (Mbps) */
	SWIFT_P_MIN_RATE_FXP20     = 16, /* rate floor / effective incast floor (FXP20) */
	/* --- endpoint window (paper §3.3); reserved + inert in v1 (fabric-only) --- */
	SWIFT_P_ENDPOINT_TARGET_NS = 17, /* ecwnd target delay (ns); phase 2 */
	SWIFT_P_ENDPOINT_EWMA_FXP16 = 18,/* ecwnd endpoint-delay EWMA weight; phase 2 */
	/* --- diagnostics (KEEP LAST) --- */
	SWIFT_P_TRACE_ON           = 19, /* runtime trace toggle (0/1); not a tuning param */
	SWIFT_NUM_PARAMS
};

/*
 * Counter indices.
 *
 * EVENTS is the total event count. AI and MD count the two Algorithm-1 control
 * branches on RTT events. DELAY_LT/GT_TARGET classify each RTT event's delay vs the
 * flow-scaled target (LT coincides with AI, GT with the MD attempt). The remaining
 * counters instrument the numeric edge cases the brief asked to watch.
 *
 * RTT_DROPPED_UNSTAMPED counts probe responses discarded because the responder did
 * not stamp t2/t4 (or the stamps were inconsistent). The paper defines no fallback
 * for unstamped responses, so neither does the algorithm: a dropped sample changes
 * nothing. A steadily climbing value with AI/MD frozen at 0 means the responder
 * cannot support Swift (see README, "The delay signal").
 */
enum swift_counter_idx {
	SWIFT_C_EVENTS             = 0,  /* total events dispatched */
	SWIFT_C_AI                 = 1,  /* additive-increase applications */
	SWIFT_C_MD                 = 2,  /* multiplicative-decrease applications */
	SWIFT_C_DELAY_LT_TARGET    = 3,  /* RTT events with delay < target */
	SWIFT_C_DELAY_GT_TARGET    = 4,  /* RTT events with delay >= target */
	SWIFT_C_CWND_BELOW_ONE     = 5,  /* events leaving cwnd < 1 pkt (pacing regime) */
	SWIFT_C_FLOW_SCALE_SAT     = 6,  /* fs_term saturated to 0 or fs_range */
	SWIFT_C_CAN_DECREASE_BLOCK = 7,  /* MD wanted but blocked by once-per-RTT guard */
	SWIFT_C_RATE_CLAMPED_MIN   = 8,  /* actuated rate clamped up to min_rate floor */
	SWIFT_C_RATE_CLAMPED_MAX   = 9,  /* actuated rate clamped down to MAX_RATE */
	SWIFT_C_NACK_MD            = 10, /* NACK-triggered multiplicative decrease */
	SWIFT_C_NACK_RESET         = 11, /* NACK retx threshold -> cwnd=min_cwnd */
	SWIFT_C_RTT_DROPPED_UNSTAMPED = 12, /* RTT samples dropped: responder t2/t4 missing/inconsistent */
	SWIFT_NUM_COUNTERS
};

struct swift_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* THE contract. Edit only here.
 *
 * Honesty caveat (per brief §7): the Swift paper publishes formula structure and
 * parameter NAMES (Table 2 / §3.5) but NOT a canonical numeric tuning table. The only
 * concrete paper values: min_cwnd = 0.001 pkt (§3.4); base_target ~25 us (stack test)
 * and 50 us fabric / 100 us endpoint (storage test). ai=1, beta~0.8, max_mdf=0.5 are
 * typical/community values, not authoritative. EVERY tuning default below is therefore
 * a starting seed to be swept on hardware; see the FIXME notes in swift_algo_params.h.
 *
 * Critical retune lesson carried from TIMELY: the paper base_target (~25 us) is for
 * Google's fabric; idle fabric RTT on this BF3 setup measured ~3-4 us, so base_target
 * must be set from measurement (~6-10 us start) or the controller never decreases.
 *
 * FXP16 cwnd: 0.001=66, 0.5=32768, 1.0=65536, 16=1048576, 64=4194304.
 */
static const struct swift_param_desc swift_params[SWIFT_NUM_PARAMS] = {
	[SWIFT_P_BASE_TARGET_NS]      = {"base_target_ns",      "ns",        10000,       1,   500000},
	[SWIFT_P_FS_RANGE_NS]         = {"fs_range_ns",         "ns",        15000,       1,   500000},
	[SWIFT_P_FS_MIN_CWND]         = {"fs_min_cwnd",         "FXP16pkt",  32768,       1, 67108864},
	[SWIFT_P_FS_MAX_CWND]         = {"fs_max_cwnd",         "FXP16pkt", 1048576,      1, 67108864},
	[SWIFT_P_NUM_HOPS]            = {"num_hops",            "count",         1,       1,       64},
	[SWIFT_P_HOP_SCALE_NS]        = {"hop_scale_ns",        "ns",            0,       0,   100000},
	[SWIFT_P_AI_FXP16]            = {"ai",                  "FXP16pkt",  65536,       1,  6553600},
	[SWIFT_P_MD_BETA_FXP16]       = {"md_beta",             "FXP16",     52429,       1,    65535},
	[SWIFT_P_MAX_MDF_FXP16]       = {"max_mdf",             "FXP16",     32768,       1,    65535},
	[SWIFT_P_MIN_CWND]            = {"min_cwnd",            "FXP16pkt",     66,       1,    65536},
	[SWIFT_P_MAX_CWND]            = {"max_cwnd",            "FXP16pkt", 4194304,   65536,134217728},
	[SWIFT_P_INIT_CWND]           = {"init_cwnd",           "FXP16pkt", 1048576,      1,134217728},
	[SWIFT_P_RETX_RESET_THRESH]   = {"retx_reset_thresh",   "count",         3,       1,      100},
	[SWIFT_P_MSS_BYTES]           = {"mss_bytes",           "bytes",      4096,     256,     9000},
	[SWIFT_P_MIN_RTT_NS]          = {"min_rtt_ns",          "ns",         3800,     500,   100000},
	[SWIFT_P_LINE_RATE_MBPS]      = {"line_rate_mbps",      "Mbps",     100000,    1000,   800000},
	[SWIFT_P_MIN_RATE_FXP20]      = {"min_rate",            "FXP20",        64,       1,   104858},
	[SWIFT_P_ENDPOINT_TARGET_NS]  = {"endpoint_target_ns",  "ns",       100000,       1,  2000000},
	[SWIFT_P_ENDPOINT_EWMA_FXP16] = {"endpoint_ewma",       "FXP16",     32768,       1,    65535},
	/* Diagnostics-only: gates runtime device-trace emission. def=0 (quiet), range [0,1].
	 * min MUST be 0 so traces can be turned back off at runtime. */
	[SWIFT_P_TRACE_ON]            = {"trace_on",            "bool",          0,       0,        1},
};

static const char *const swift_counter_names[SWIFT_NUM_COUNTERS] = {
	[SWIFT_C_EVENTS]             = "events",
	[SWIFT_C_AI]                 = "ai",
	[SWIFT_C_MD]                 = "md",
	[SWIFT_C_DELAY_LT_TARGET]    = "delay_lt_target",
	[SWIFT_C_DELAY_GT_TARGET]    = "delay_gt_target",
	[SWIFT_C_CWND_BELOW_ONE]     = "cwnd_below_one",
	[SWIFT_C_FLOW_SCALE_SAT]     = "flow_scale_saturated",
	[SWIFT_C_CAN_DECREASE_BLOCK] = "can_decrease_blocked",
	[SWIFT_C_RATE_CLAMPED_MIN]   = "rate_clamped_min",
	[SWIFT_C_RATE_CLAMPED_MAX]   = "rate_clamped_max",
	[SWIFT_C_NACK_MD]            = "nack_md",
	[SWIFT_C_NACK_RESET]         = "nack_reset",
	[SWIFT_C_RTT_DROPPED_UNSTAMPED] = "rtt_dropped_unstamped",
};

#endif /* SWIFT_PARAMS_H_ */
