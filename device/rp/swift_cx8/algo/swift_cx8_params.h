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

#ifndef SWIFT_CX8_PARAMS_H_
#define SWIFT_CX8_PARAMS_H_

#include <stdint.h>

/*
 * Single source of truth for Swift-CX8's PPCC parameter and counter contract. Included
 * by swift_cx8.c (DPA registration) and swift_cx8_params_dump.c (host JSON dump for
 * incast-bench).
 *
 * Wire encodings (consistent with the timely/dcqcn contracts):
 *   time -> ns (uint32); cwnd -> FXP16 packets (1<<16 = 1.0); ratios/gains -> FXP16;
 *   rate -> FXP20 (1<<20 = 100% line rate).
 * Human-unit conversion is the orchestrator's job, not the device's.
 *
 * Defaults match the compile-time #defines in swift_cx8_algo_params.h. The flow-scaling
 * coefficients alpha/beta are derived on-device and intentionally not exposed.
 */

/* Project algo-ID registry: rtt_template=0xBFFF, DCQCN=0xBFFE, TIMELY=0xBFFD,
 * Swift=0xBFFC, SMaRTT=0xBFFB. The tables below are identical to Swift's -- only the
 * ID differs, so incast-bench can drive either with the same index map. */
#define SWIFT_CX8_ALGO_ID  0xBFFA

/*
 * Parameter indices. APPEND-ONLY: orchestrators address params by index, so never
 * reorder or insert. TRACE_ON is diagnostics-only (runtime trace toggle, see
 * swift_cx8.h) and stays LAST.
 */
enum swift_cx8_param_idx {
	/* --- target delay scaling (paper §3.5) --- */
	SWIFT_CX8_P_BASE_TARGET_NS     = 0,  /* base fabric target; absorbs serialization/propagation/fixed delay */
	SWIFT_CX8_P_FS_RANGE_NS        = 1,  /* fs_range: max additional target for small flows */
	SWIFT_CX8_P_FS_MIN_CWND        = 2,  /* fs_min_cwnd: low knee, cwnd<=this -> fs_term=fs_range (FXP16 pkts) */
	SWIFT_CX8_P_FS_MAX_CWND        = 3,  /* fs_max_cwnd: high knee, cwnd>=this -> fs_term=0 (FXP16 pkts) */
	SWIFT_CX8_P_NUM_HOPS           = 4,  /* fixed #hops (BF3 TTL unreadable); per-hop term = num_hops*hop_scale */
	SWIFT_CX8_P_HOP_SCALE_NS       = 5,  /* hbar: per-hop target add; 0 on BF3 */
	/* --- AIMD on cwnd (paper Algorithm 1) --- */
	SWIFT_CX8_P_AI_FXP16           = 6,  /* ai: additive increment per RTT event (FXP16 pkts) */
	SWIFT_CX8_P_MD_BETA_FXP16      = 7,  /* beta: MD gain on (delay-target)/delay */
	SWIFT_CX8_P_MAX_MDF_FXP16      = 8,  /* max_mdf: max multiplicative decrease per event / on loss */
	SWIFT_CX8_P_MIN_CWND           = 9,  /* min_cwnd floor (FXP16 pkts); paper §3.4 = 0.001 pkt = 66 */
	SWIFT_CX8_P_MAX_CWND           = 10, /* max_cwnd ceiling (FXP16 pkts), near/above BDP */
	SWIFT_CX8_P_INIT_CWND          = 11, /* new-flow starting window (FXP16 pkts) */
	SWIFT_CX8_P_RETX_RESET_THRESH  = 12, /* consecutive NACKs -> cwnd=min_cwnd (paper §3.6) */
	/* --- cwnd -> rate actuation (BDP identity) --- */
	SWIFT_CX8_P_MSS_BYTES          = 13, /* segment size for BDP (bytes) */
	SWIFT_CX8_P_MIN_RTT_NS         = 14, /* base RTT for BDP + can_decrease/pacing reference (ns) */
	SWIFT_CX8_P_LINE_RATE_MBPS     = 15, /* NIC line rate for BDP (Mbps) */
	SWIFT_CX8_P_MIN_RATE_FXP20     = 16, /* rate floor / effective incast floor (FXP20) */
	/* --- endpoint window (paper §3.3); reserved + inert in v1 (fabric-only) --- */
	SWIFT_CX8_P_ENDPOINT_TARGET_NS = 17, /* ecwnd target delay (ns); phase 2 */
	SWIFT_CX8_P_ENDPOINT_EWMA_FXP16 = 18,/* ecwnd endpoint-delay EWMA weight; phase 2 */
	/* --- diagnostics (KEEP LAST) --- */
	SWIFT_CX8_P_TRACE_ON           = 19, /* runtime trace toggle (0/1); not a tuning param */
	SWIFT_CX8_NUM_PARAMS
};

/*
 * Counter indices. AI/MD count the two Algorithm-1 branches; DELAY_LT/GT_TARGET
 * classify each RTT event vs the flow-scaled target; the rest instrument numeric edge
 * cases. RTT_DROPPED_UNSTAMPED counts probe responses discarded for missing or
 * inconsistent responder stamps; a dropped sample changes nothing (see the Swift
 * README, "The delay signal").
 */
enum swift_cx8_counter_idx {
	SWIFT_CX8_C_EVENTS             = 0,  /* total events dispatched */
	SWIFT_CX8_C_AI                 = 1,  /* additive-increase applications */
	SWIFT_CX8_C_MD                 = 2,  /* multiplicative-decrease applications */
	SWIFT_CX8_C_DELAY_LT_TARGET    = 3,  /* RTT events with delay < target */
	SWIFT_CX8_C_DELAY_GT_TARGET    = 4,  /* RTT events with delay >= target */
	SWIFT_CX8_C_CWND_BELOW_ONE     = 5,  /* events leaving cwnd < 1 pkt (pacing regime) */
	SWIFT_CX8_C_FLOW_SCALE_SAT     = 6,  /* fs_term saturated to 0 or fs_range */
	SWIFT_CX8_C_CAN_DECREASE_BLOCK = 7,  /* MD wanted but blocked by once-per-RTT guard */
	SWIFT_CX8_C_RATE_CLAMPED_MIN   = 8,  /* actuated rate clamped up to min_rate floor */
	SWIFT_CX8_C_RATE_CLAMPED_MAX   = 9,  /* actuated rate clamped down to MAX_RATE */
	SWIFT_CX8_C_NACK_MD            = 10, /* NACK-triggered multiplicative decrease */
	SWIFT_CX8_C_NACK_RESET         = 11, /* NACK retx threshold -> cwnd=min_cwnd */
	SWIFT_CX8_C_RTT_DROPPED_UNSTAMPED = 12, /* RTT samples dropped: responder t2/t4 missing/inconsistent */
	SWIFT_CX8_NUM_COUNTERS
};

struct swift_cx8_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* THE contract -- edit only here. Every tuning default is a seed (see the FIXMEs in
 * swift_cx8_algo_params.h); the paper fixes only min_cwnd = 0.001 pkt.
 * FXP16 cwnd: 0.001=66, 0.5=32768, 1.0=65536, 16=1048576, 64=4194304. */
static const struct swift_cx8_param_desc swift_cx8_params[SWIFT_CX8_NUM_PARAMS] = {
	[SWIFT_CX8_P_BASE_TARGET_NS]      = {"base_target_ns",      "ns",        10000,       1,   500000},
	[SWIFT_CX8_P_FS_RANGE_NS]         = {"fs_range_ns",         "ns",        15000,       1,   500000},
	[SWIFT_CX8_P_FS_MIN_CWND]         = {"fs_min_cwnd",         "FXP16pkt",  32768,       1, 67108864},
	[SWIFT_CX8_P_FS_MAX_CWND]         = {"fs_max_cwnd",         "FXP16pkt", 1048576,      1, 67108864},
	[SWIFT_CX8_P_NUM_HOPS]            = {"num_hops",            "count",         1,       1,       64},
	[SWIFT_CX8_P_HOP_SCALE_NS]        = {"hop_scale_ns",        "ns",            0,       0,   100000},
	[SWIFT_CX8_P_AI_FXP16]            = {"ai",                  "FXP16pkt",  65536,       1,  6553600},
	[SWIFT_CX8_P_MD_BETA_FXP16]       = {"md_beta",             "FXP16",     52429,       1,    65535},
	[SWIFT_CX8_P_MAX_MDF_FXP16]       = {"max_mdf",             "FXP16",     32768,       1,    65535},
	[SWIFT_CX8_P_MIN_CWND]            = {"min_cwnd",            "FXP16pkt",     66,       1,    65536},
	[SWIFT_CX8_P_MAX_CWND]            = {"max_cwnd",            "FXP16pkt", 4194304,   65536,134217728},
	[SWIFT_CX8_P_INIT_CWND]           = {"init_cwnd",           "FXP16pkt", 1048576,      1,134217728},
	[SWIFT_CX8_P_RETX_RESET_THRESH]   = {"retx_reset_thresh",   "count",         3,       1,      100},
	[SWIFT_CX8_P_MSS_BYTES]           = {"mss_bytes",           "bytes",      4096,     256,     9000},
	[SWIFT_CX8_P_MIN_RTT_NS]          = {"min_rtt_ns",          "ns",         3800,     500,   100000},
	[SWIFT_CX8_P_LINE_RATE_MBPS]      = {"line_rate_mbps",      "Mbps",     100000,    1000,   800000},
	[SWIFT_CX8_P_MIN_RATE_FXP20]      = {"min_rate",            "FXP20",        64,       1,   104858},
	[SWIFT_CX8_P_ENDPOINT_TARGET_NS]  = {"endpoint_target_ns",  "ns",       100000,       1,  2000000},
	[SWIFT_CX8_P_ENDPOINT_EWMA_FXP16] = {"endpoint_ewma",       "FXP16",     32768,       1,    65535},
	/* min MUST be 0 so traces can be turned back off at runtime. */
	[SWIFT_CX8_P_TRACE_ON]            = {"trace_on",            "bool",          0,       0,        1},
};

static const char *const swift_cx8_counter_names[SWIFT_CX8_NUM_COUNTERS] = {
	[SWIFT_CX8_C_EVENTS]             = "events",
	[SWIFT_CX8_C_AI]                 = "ai",
	[SWIFT_CX8_C_MD]                 = "md",
	[SWIFT_CX8_C_DELAY_LT_TARGET]    = "delay_lt_target",
	[SWIFT_CX8_C_DELAY_GT_TARGET]    = "delay_gt_target",
	[SWIFT_CX8_C_CWND_BELOW_ONE]     = "cwnd_below_one",
	[SWIFT_CX8_C_FLOW_SCALE_SAT]     = "flow_scale_saturated",
	[SWIFT_CX8_C_CAN_DECREASE_BLOCK] = "can_decrease_blocked",
	[SWIFT_CX8_C_RATE_CLAMPED_MIN]   = "rate_clamped_min",
	[SWIFT_CX8_C_RATE_CLAMPED_MAX]   = "rate_clamped_max",
	[SWIFT_CX8_C_NACK_MD]            = "nack_md",
	[SWIFT_CX8_C_NACK_RESET]         = "nack_reset",
	[SWIFT_CX8_C_RTT_DROPPED_UNSTAMPED] = "rtt_dropped_unstamped",
};

#endif /* SWIFT_CX8_PARAMS_H_ */
