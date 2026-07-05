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
 * Single source of truth for SMaRTT's PPCC parameter and counter contract.
 *
 * Included by:
 *   - smartt_reps.c (DPA): smartt_reps_init() registers params/counters from
 *     smartt_reps_params[].
 *   - smartt_reps_params_dump.c (host): emits JSON for incast-bench's parameter encoder.
 *
 * SMaRTT (Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed &
 * Timed Transport with Recycled Entropies", arXiv:2404.01630, Apr 2024) is a
 * sender-based, WINDOW-based controller combining a delay (RTT) signal and an ECN
 * signal per ACK (§3.2, four-case control loop), with QuickAdapt (§3.3) for severe
 * congestion and FastIncrease (§3.4) for bandwidth reclamation, gated by
 * Wait-to-Decrease (§3.6.1). REPS (§3.6, per-packet load balancing) is intentionally
 * NOT implemented -- BF3 exposes no per-packet entropy hook and the testbed has no
 * meaningful multipath (see README "REPS is intentionally NOT implemented").
 *
 * We control a congestion window (cwnd, in FXP16 packets) and actuate it on
 * BlueField-3 by mapping cwnd -> Tx rate via the BDP identity (see smartt_reps.c). BF3
 * has no usable credit/window actuator (doca_pcc_dev_results_t.credits is unbacked), so
 * rate is the actuator; cwnd remains the control variable.
 *
 * Wire encodings (consistent with the timely/dcqcn/swift contracts in this repo):
 *   - time          -> nanoseconds (uint32)
 *   - cwnd          -> FXP16 packets (1<<16 = 1.0 packet = 1 MSS/MTU; 0.001 pkt = 66)
 *   - ratios/gains  -> FXP16 (1<<16 = 1.0): fd, md, fi, mi, qa_scaling, thresholds
 *   - rate          -> FXP20 (1<<20 = 100% line rate): min_rate
 * Off-host tooling (incast-bench) is responsible for any human-unit -> FXP conversion.
 *
 * Defaults are aligned with the compile-time #defines in smartt_reps_algo_params.h so
 * that a doca_pcc instance running with no param writes behaves identically to a build
 * with the contract bypassed. The orchestrator overrides these at runtime over the PPCC
 * channel (cmd_type=0xB) when sweeping the parameter space.
 */

/* SMaRTT algorithm ID. Project convention: TIMELY=0xBFFD, DCQCN=0xBFFE,
 * rtt_template/template=0xBFFF, Swift=0xBFFC. SMaRTT claims the next-lower free slot. */
#define SMARTT_REPS_ALGO_ID  0xBFFB

/*
 * Parameter indices.
 *
 * APPEND-ONLY: external orchestrators (incast-bench) address parameters by index in
 * bulk-set, so never reorder or insert -- only append before TRACE_ON is reassigned.
 * TRACE_ON is kept LAST so indices 0..N-2 stay stable across versions.
 *
 * Grouping: [0..1] delay reference (§3.5); [2..6] four-case + QuickAdapt gains
 * (§3.2, §3.3); [7..8] FastIncrease (§3.4); [9..11] Wait-to-Decrease + ECN-hold
 * (§3.6.1, signal mapping); [12..14] cwnd bounds (§3.5); [15..17] cwnd->rate actuation
 * (BDP); [18] diagnostics.
 *
 * TRACE_ON is a diagnostics-only toggle, NOT a tuning knob: it gates runtime emission
 * of the format 20/21/22/23/24 device traces (see smartt_reps.h). It rides in the param
 * array so it can be flipped at runtime over the existing PPCC channel without
 * rebuilding.
 */
enum smartt_reps_param_idx {
	/* --- delay reference (paper §3.5) --- */
	SMARTT_REPS_P_BASE_RTT_NS         = 0,  /* brtt: no-load fabric RTT (measured ~3.8 us here) */
	SMARTT_REPS_P_TARGET_RTT_NS       = 1,  /* trtt: congestion threshold = 1.5*brtt (§3.5) */
	/* --- four-case gains (paper §3.2) + QuickAdapt (§3.3) --- */
	SMARTT_REPS_P_FD_FXP16            = 2,  /* fd: Fair Decrease gain (§3.2.1 Eq.1) */
	SMARTT_REPS_P_MD_FXP16            = 3,  /* md: Multiplicative Decrease gain (§3.2.2 Eq.2) */
	SMARTT_REPS_P_FI_FXP16            = 4,  /* fi: Fair Increase gain (§3.2.3 Eq.3) */
	SMARTT_REPS_P_MI_FXP16            = 5,  /* mi: Multiplicative Increase gain (§3.2.4 Eq.4); 0 => derive */
	SMARTT_REPS_P_QA_SCALING_FXP16    = 6,  /* qa_scaling: QuickAdapt target scale (§3.3) */
	/* --- FastIncrease (paper §3.4) --- */
	SMARTT_REPS_P_FAST_INC_K_FXP16    = 7,  /* fast_inc_k: MTUs added per coalesced ACK (§3.4) */
	SMARTT_REPS_P_FAST_INC_THRESH_FXP16 = 8,/* fast_inc_thresh: rtt<=brtt*thresh tolerance (§3.4) */
	/* --- Wait-to-Decrease + ECN-hold (paper §3.6.1, signal mapping) --- */
	SMARTT_REPS_P_WTD_ALPHA_FXP16     = 9,  /* wtd_alpha: EWMA weight for avg_wtd (§3.6.1) */
	SMARTT_REPS_P_WTD_THRESH_FXP16    = 10, /* wtd_thresh: avg_wtd gate; 0 disables WTD (§3.6.1) */
	SMARTT_REPS_P_ECN_CNP_HOLD_NS     = 11, /* ecn_cnp_hold_ns: CNP->ECN assertion window */
	/* --- cwnd bounds (paper §3.5) --- */
	SMARTT_REPS_P_MAX_CWND_FRAC_FXP16 = 12, /* max_cwnd_frac: cwnd cap as fraction of BDP (§3.5) */
	SMARTT_REPS_P_MIN_CWND_FXP16      = 13, /* min_cwnd: cwnd floor, FXP16 pkts (§3.5) */
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
 * Counter indices.
 *
 * EVENTS is the total event count. ACK_EVENTS is the control-loop driver and MUST be
 * nonzero on this hardware (SMaRTT is the repo's first ACK-event consumer). The FD/MD/
 * FI/MI counters count the four Algorithm-1 window branches (FD includes the FD applied
 * inside MD; FI includes the FI applied inside MI, per §3.2.2/§3.2.4). The remaining
 * counters instrument QuickAdapt, FastIncrease, WTD gating, the post-QA blackout, the
 * rate clamps, and the ΔPSN sanity-cap fallbacks.
 */
enum smartt_reps_counter_idx {
	SMARTT_REPS_C_EVENTS          = 0,  /* total events dispatched */
	SMARTT_REPS_C_ACK_EVENTS      = 1,  /* ACK events (control-loop driver -- must be nonzero!) */
	SMARTT_REPS_C_RTT_EVENTS      = 2,  /* RTT probe responses */
	SMARTT_REPS_C_CNP_EVENTS      = 3,  /* CNP events ("is ECN marking live?") */
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
	SMARTT_REPS_C_PSN_ANOMALY     = 15, /* ΔPSN sanity-cap fallbacks to num_coalesced */
	SMARTT_REPS_NUM_COUNTERS
};

struct smartt_reps_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* THE contract. Edit only here.
 *
 * Honesty caveat: SMaRTT (Bonato et al., arXiv:2404.01630) publishes formula structure
 * and the §3.5 gain values (fd=0.8, md=2.0, fi=0.25, qa_scaling=0.8, k=2 MTU), but
 * several tuning constants are unstated in the paper and are FIXME seeds to sweep:
 * fast_inc_thresh (paper says "close to brtt"), wtd_alpha (§3.6.1 gives no α),
 * ecn_cnp_hold_ns (a BF3-specific CNP->ECN assertion window). mi=0 is a sentinel: the
 * device derives mi = brtt/(trtt-brtt) (= 2.0 at trtt=1.5*brtt, §3.2.4).
 *
 * Critical retune lesson carried from TIMELY/Swift: idle fabric RTT on this BF3 setup
 * measured ~3.8 us (NOT the paper's 12 us), so base_rtt_ns=4000 / target_rtt_ns=6000;
 * retune from a no-load fabric_rtt trace (format 23) or the controller never decreases.
 *
 * γ note (§3.5 "Scaling"): the paper scales fi/mi by γ=bdp/reference_bdp
 * (reference = 100 Gbps × 12 µs). This fabric's BDP is ~1/3 of that; the device does
 * NOT auto-scale, so the defaults keep the un-scaled §3.5 values and the sweep explores
 * both regimes. fd/md are fixed regardless of link speed (paper: always react strongly).
 *
 * FXP16 cwnd: 0.001=66, 0.5=32768, 1.0=65536, 16=1048576, 64=4194304.
 */
static const struct smartt_reps_param_desc smartt_reps_params[SMARTT_REPS_NUM_PARAMS] = {
	[SMARTT_REPS_P_BASE_RTT_NS]         = {"base_rtt_ns",      "ns",          4000,     500,    500000},
	[SMARTT_REPS_P_TARGET_RTT_NS]       = {"target_rtt_ns",    "ns",          6000,     600,    750000},
	[SMARTT_REPS_P_FD_FXP16]            = {"fd",               "FXP16",      52429,       0,    655360},
	[SMARTT_REPS_P_MD_FXP16]            = {"md",               "FXP16",     131072,       0,   1310720},
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
	/* Diagnostics-only: gates runtime device-trace emission. def=0 (quiet), range [0,1].
	 * min MUST be 0 so traces can be turned back off at runtime. */
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
