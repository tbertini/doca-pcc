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

#ifndef SMARTT_REPS_PARAMS_H_
#define SMARTT_REPS_PARAMS_H_

#include <stdint.h>

/*
 * PPCC parameter and counter contract for SMaRTT: the single source of truth shared by
 * the device algorithm (smartt_reps.c) and the host contract dumper
 * (host/smartt_reps_params_dump.c).
 *
 * SMaRTT is from Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting
 * Trimmed & Timed Transport with Recycled Entropies", arXiv:2404.01630, 2024. REPS
 * (Section 3.6) is not implemented.
 *
 * Wire encodings: time in ns (uint32), cwnd in FXP16 packets (1<<16 = 1.0 packet =
 * 1 MSS/MTU), ratios and gains in FXP16, rate in FXP20 (1<<20 = 100% line rate).
 * Human-unit conversion is the orchestrator's job.
 */

/* SMaRTT algorithm ID. */
#define SMARTT_REPS_ALGO_ID  0xBFFB

/*
 * Parameter indices.
 *
 * APPEND-ONLY: orchestrators address parameters by index, so never reorder or insert.
 * TRACE_ON is a diagnostics toggle, not a tuning knob, and stays LAST so indices
 * 0..N-2 remain stable.
 */
enum smartt_reps_param_idx {
	/* --- delay reference (paper Section 3.5) --- */
	SMARTT_REPS_P_BASE_RTT_NS         = 0,  /* brtt: no-load fabric RTT (ns) */
	SMARTT_REPS_P_TARGET_RTT_NS       = 1,  /* trtt: congestion threshold = 1.5*brtt (Section 3.5) */
	/* --- four-case gains (paper Section 3.2) + QuickAdapt (Section 3.3) --- */
	SMARTT_REPS_P_FD_FXP16            = 2,  /* fd: Fair Decrease gain (Eq. 1, Section 3.2.1) */
	SMARTT_REPS_P_MD_FXP16            = 3,  /* md: Multiplicative Decrease gain (Eq. 2, Section 3.2.2) */
	SMARTT_REPS_P_FI_FXP16            = 4,  /* fi: Fair Increase gain (Eq. 3, Section 3.2.3) */
	SMARTT_REPS_P_MI_FXP16            = 5,  /* mi: Multiplicative Increase gain (Eq. 4, Section 3.2.4); 0 => derive */
	SMARTT_REPS_P_QA_SCALING_FXP16    = 6,  /* qa_scaling: QuickAdapt target scale (Section 3.3) */
	/* --- FastIncrease (paper Section 3.4) --- */
	SMARTT_REPS_P_FAST_INC_K_FXP16    = 7,  /* fast_inc_k: MTUs added per coalesced ACK (Section 3.4) */
	SMARTT_REPS_P_FAST_INC_THRESH_FXP16 = 8,/* fast_inc_thresh: rtt<=brtt*thresh tolerance (Section 3.4) */
	/* --- Wait-to-Decrease + ECN-hold (paper Section 3.6.1) --- */
	SMARTT_REPS_P_WTD_ALPHA_FXP16     = 9,  /* wtd_alpha: EWMA weight for avg_wtd (Section 3.6.1) */
	SMARTT_REPS_P_WTD_THRESH_FXP16    = 10, /* wtd_thresh: avg_wtd gate; 0 disables WTD (Section 3.6.1) */
	SMARTT_REPS_P_ECN_CNP_HOLD_NS     = 11, /* ecn_cnp_hold_ns: CNP->ECN assertion window (ns) */
	/* --- cwnd bounds (paper Section 3.5) --- */
	SMARTT_REPS_P_MAX_CWND_FRAC_FXP16 = 12, /* max_cwnd_frac: cwnd cap as fraction of BDP (Section 3.5) */
	SMARTT_REPS_P_MIN_CWND_FXP16      = 13, /* min_cwnd: cwnd floor, FXP16 pkts (Section 3.5) */
	SMARTT_REPS_P_INIT_CWND_FXP16     = 14, /* init_cwnd: new-flow window; 0 => BDP */
	/* --- cwnd -> rate actuation (BDP identity) --- */
	SMARTT_REPS_P_MSS_BYTES           = 15, /* segment size for BDP (bytes) */
	SMARTT_REPS_P_LINE_RATE_MBPS      = 16, /* NIC line rate for BDP (Mbps) */
	SMARTT_REPS_P_MIN_RATE_FXP20      = 17, /* rate floor (FXP20) */
	/* --- diagnostics (KEEP LAST) --- */
	SMARTT_REPS_P_TRACE_ON            = 18, /* runtime trace toggle (0/1); not a tuning param */
	SMARTT_REPS_NUM_PARAMS
};

/*
 * Counter indices. The FD/MD/FI/MI counters count the four Algorithm 1 window branches:
 * FD includes the FD applied inside MD, and FI includes the FI applied inside MI
 * (paper Sections 3.2.2 and 3.2.4).
 */
enum smartt_reps_counter_idx {
	SMARTT_REPS_C_EVENTS          = 0,  /* total events dispatched */
	SMARTT_REPS_C_ACK_EVENTS      = 1,  /* ACK events (control-loop driver) */
	SMARTT_REPS_C_RTT_EVENTS      = 2,  /* RTT probe responses */
	SMARTT_REPS_C_CNP_EVENTS      = 3,  /* CNP events (ECN-marking answer) */
	SMARTT_REPS_C_NACK_EVENTS     = 4,  /* NACK events (loss signal) */
	SMARTT_REPS_C_FD              = 5,  /* Fair Decrease applications (incl. FD inside MD) */
	SMARTT_REPS_C_MD              = 6,  /* Multiplicative Decrease applications */
	SMARTT_REPS_C_FI              = 7,  /* Fair Increase applications (incl. FI inside MI) */
	SMARTT_REPS_C_MI              = 8,  /* Multiplicative Increase applications */
	SMARTT_REPS_C_QA_FIRED        = 9,  /* QuickAdapt window resets */
	SMARTT_REPS_C_FAST_INC        = 10, /* ACK events in active FastIncrease */
	SMARTT_REPS_C_WTD_BLOCKED     = 11, /* ECN-decrease wanted but avg_wtd < thresh */
	SMARTT_REPS_C_ACKS_IGNORED    = 12, /* ACK events swallowed by post-QA blackout */
	SMARTT_REPS_C_RATE_CLAMPED_MIN = 13,/* actuated rate hit min_rate */
	SMARTT_REPS_C_RATE_CLAMPED_MAX = 14,/* actuated rate hit MAX_RATE */
	SMARTT_REPS_C_PSN_ANOMALY     = 15, /* delta-PSN sanity-cap fallbacks to num_coalesced */
	SMARTT_REPS_NUM_COUNTERS
};

struct smartt_reps_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* The contract: parameter name, unit, default, min and max. Edit only here.
 * mi = 0 is a sentinel: the device derives mi = brtt/(trtt-brtt) (paper Section 3.2.4).
 * FXP16 cwnd: 0.001=66, 0.5=32768, 1.0=65536, 16=1048576, 64=4194304.
 */
static const struct smartt_reps_param_desc smartt_reps_params[SMARTT_REPS_NUM_PARAMS] = {
	[SMARTT_REPS_P_BASE_RTT_NS]         = {"base_rtt_ns",      "ns",          4000,     500,    500000},
	[SMARTT_REPS_P_TARGET_RTT_NS]       = {"target_rtt_ns",    "ns",          6000,     600,    750000},
	[SMARTT_REPS_P_FD_FXP16]            = {"fd",               "FXP16",      52429,       0,    655360},
	[SMARTT_REPS_P_MD_FXP16]            = {"md",               "FXP16",     262144,       0,   1310720},
	[SMARTT_REPS_P_FI_FXP16]            = {"fi",               "FXP16",      16384,       0,    655360},
	[SMARTT_REPS_P_MI_FXP16]            = {"mi",               "FXP16",          0,       0,   1310720},
	[SMARTT_REPS_P_QA_SCALING_FXP16]    = {"qa_scaling",       "FXP16",      52429,       1,    131072},
	[SMARTT_REPS_P_FAST_INC_K_FXP16]    = {"fast_inc_k",       "FXP16pkt",  131072,       0,    655360},
	[SMARTT_REPS_P_FAST_INC_THRESH_FXP16] = {"fast_inc_thresh","FXP16",      72090,   65536,    262144},
	[SMARTT_REPS_P_WTD_ALPHA_FXP16]     = {"wtd_alpha",        "FXP16",       8192,       0,     65536},
	[SMARTT_REPS_P_WTD_THRESH_FXP16]    = {"wtd_thresh",       "FXP16",      16384,       0,     65536},
	[SMARTT_REPS_P_ECN_CNP_HOLD_NS]     = {"ecn_cnp_hold_ns",  "ns",         10000,       0,  10000000},
	[SMARTT_REPS_P_MAX_CWND_FRAC_FXP16] = {"max_cwnd_frac",    "FXP16",      81920,   16384,    655360},
	[SMARTT_REPS_P_MIN_CWND_FXP16]      = {"min_cwnd",         "FXP16pkt",   65536,      66,   6553600},
	[SMARTT_REPS_P_INIT_CWND_FXP16]     = {"init_cwnd",        "FXP16pkt",       0,       0, 134217728},
	[SMARTT_REPS_P_MSS_BYTES]           = {"mss_bytes",        "bytes",       4096,     256,      9000},
	[SMARTT_REPS_P_LINE_RATE_MBPS]      = {"line_rate_mbps",   "Mbps",      100000,    1000,    800000},
	[SMARTT_REPS_P_MIN_RATE_FXP20]      = {"min_rate",         "FXP20",         64,       1,    104858},
	/* Diagnostics only: gates runtime device-trace emission. min must stay 0 so traces
	 * can be turned back off at runtime. */
	[SMARTT_REPS_P_TRACE_ON]            = {"trace_on",         "bool",           0,       0,         1},
};

static const char *const smartt_reps_counter_names[SMARTT_REPS_NUM_COUNTERS] = {
	[SMARTT_REPS_C_EVENTS]           = "events",
	[SMARTT_REPS_C_ACK_EVENTS]       = "ack_events",
	[SMARTT_REPS_C_RTT_EVENTS]       = "rtt_events",
	[SMARTT_REPS_C_CNP_EVENTS]       = "cnp_events",
	[SMARTT_REPS_C_NACK_EVENTS]      = "nack_events",
	[SMARTT_REPS_C_FD]               = "fd_applied",
	[SMARTT_REPS_C_MD]               = "md_applied",
	[SMARTT_REPS_C_FI]               = "fi_applied",
	[SMARTT_REPS_C_MI]               = "mi_applied",
	[SMARTT_REPS_C_QA_FIRED]         = "qa_fired",
	[SMARTT_REPS_C_FAST_INC]         = "fast_inc_acks",
	[SMARTT_REPS_C_WTD_BLOCKED]      = "wtd_blocked",
	[SMARTT_REPS_C_ACKS_IGNORED]     = "acks_ignored",
	[SMARTT_REPS_C_RATE_CLAMPED_MIN] = "rate_clamped_min",
	[SMARTT_REPS_C_RATE_CLAMPED_MAX] = "rate_clamped_max",
	[SMARTT_REPS_C_PSN_ANOMALY]      = "psn_anomaly",
};

#endif /* SMARTT_REPS_PARAMS_H_ */
