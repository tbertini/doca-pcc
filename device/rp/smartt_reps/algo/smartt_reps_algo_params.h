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

#ifndef _SMARTT_REPS_ALGO_PARAMS_H_
#define _SMARTT_REPS_ALGO_PARAMS_H_

/*
 * Compile-time defaults for SMaRTT. These mirror the smartt_reps_params[] descriptor
 * table (smartt_reps_params.h) and are the values a flow uses until the orchestrator
 * pushes a PPCC param write. Each value cites a paper section/equation or is flagged
 * FIXME.
 *
 * Reference: Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed &
 * Timed Transport with Recycled Entropies", arXiv:2404.01630 (Apr 2024).
 *
 * NB: SMaRTT publishes the §3.5 gain values (fd, md, fi, qa_scaling, k) but leaves
 * several tuning constants unstated (fast_inc_thresh, wtd_alpha, ecn_cnp_hold_ns) --
 * those carry FIXME and are seeds to sweep on hardware.
 */

/* ===== Fixed-point conventions (match TIMELY/DCQCN/Swift) ===== */
#define SMARTT_REPS_FXP16_ONE     (1 << 16)   /* 1.0 in FXP16 (ratios, gains, cwnd packets) */
#define SMARTT_REPS_FXP16_SHIFT   (16)
#define SMARTT_REPS_RATE_MAX      (1 << 20)   /* 100% line rate in FXP20 (== DOCA_PCC_DEV_MAX_RATE) */
#define SMARTT_REPS_MTU_FXP16     (1 << 16)   /* one MTU/MSS == 1.0 packet (cwnd unit) */

/* ===== Delay reference (paper §3.5) ===== */

/*
 * base_rtt (brtt): the no-load fabric RTT (ns).
 *
 * Paper §3.5 uses brtt as the delay baseline; the four-case loop compares the measured
 * fabric RTT against brtt and trtt. Paper reference fabric brtt ~ 12 us.
 *
 * FIXME: that is the paper's fabric. Idle fabric RTT on this BF3 setup measured ~3.8 us
 * (cf. TIMELY/Swift min_rtt 3800 ns). RETUNE from a no-load fabric_rtt trace (format 23)
 * or the controller never decreases.
 */
#define SMARTT_REPS_BASE_RTT_NS  (4000)

/*
 * target_rtt (trtt): the congestion threshold (ns).
 *
 * Paper §3.5: trtt = 1.5 * brtt is the delay above which the loop treats the queue as
 * building. 1.5 * 4000 = 6000.
 */
#define SMARTT_REPS_TARGET_RTT_NS  (6000)

/* ===== Four-case gains (paper §3.2) ===== */

/*
 * fd: Fair Decrease gain (FXP16). Paper §3.2.1 Eq.1:
 *   cwnd -= (cwnd/bdp) * fd * p.size
 * §3.5 tuned value on a 100G/4KiB fabric is 0.8 (= this fabric's regime). 0.8 = 52429.
 */
#define SMARTT_REPS_FD_FXP16  (52429)  /* 0.8 (§3.5) */

/*
 * md: Multiplicative Decrease gain (FXP16). Paper §3.2.2 Eq.2:
 *   cwnd -= min( ((rtt-trtt)/rtt) * md * p.size , p.size )   ; then additionally FD
 * §3.2.2 value is 2.0 = 131072.
 *
 * FIXME: §4.2 (timeout/trimming mode -- our NACK path) suggests DOUBLING md to 2-4;
 * sweep 2.0-4.0.
 */
#define SMARTT_REPS_MD_FXP16  (131072)  /* 2.0 (§3.2.2); sweep 2-4 in NACK mode (§4.2) */

/*
 * fi: Fair Increase gain (FXP16). Paper §3.2.3 Eq.3:
 *   cwnd += (p.size/cwnd) * mtu * fi
 * §3.5 value is 0.25 = 16384.
 *
 * FIXME: §3.5 "Scaling" multiplies fi by γ = bdp/reference_bdp (reference = 100G × 12us).
 * This fabric's BDP is ~1/3 of reference, so a paper-faithful seed would be ~0.083; the
 * device does NOT auto-scale, so this keeps the un-scaled §3.5 value and the sweep
 * explores both regimes.
 */
#define SMARTT_REPS_FI_FXP16  (16384)  /* 0.25 (§3.5) */

/*
 * mi: Multiplicative Increase gain (FXP16). Paper §3.2.4 Eq.4:
 *   cwnd += min( ((trtt-rtt)/rtt) * (p.size/cwnd) * mtu * mi , p.size )  ; then FI
 *
 * SENTINEL 0: when mi == 0 the device derives mi = brtt/(trtt-brtt) on the fly
 * (§3.2.4; = 2.0 at trtt = 1.5*brtt). This keeps mi self-consistent with brtt/trtt as
 * those are retuned. Set a nonzero override to pin mi. Same γ-scaling note as fi.
 */
#define SMARTT_REPS_MI_FXP16  (0)  /* 0 => derive brtt/(trtt-brtt) on device (§3.2.4) */

/*
 * qa_scaling: QuickAdapt target scale (FXP16). Paper §3.3 Algorithm 2:
 *   cwnd = max(acked, mtu) * qa_scaling
 * §3.5 value is 0.8 = 52429.
 */
#define SMARTT_REPS_QA_SCALING_FXP16  (52429)  /* 0.8 (§3.3/§3.5) */

/* ===== FastIncrease (paper §3.4, Algorithm 3) ===== */

/*
 * fast_inc_k: MTUs added per coalesced ACK arrival while FastIncrease is active
 * (FXP16 packets). Paper §3.5: k = 2 MTU per ACK. 2.0 = 131072.
 */
#define SMARTT_REPS_FAST_INC_K_FXP16  (131072)  /* k = 2 MTU per ACK (§3.4/§3.5) */

/*
 * fast_inc_thresh: rtt "close to brtt" tolerance (FXP16). FastIncrease engages when
 *   rtt <= brtt * fast_inc_thresh.
 *
 * FIXME: the paper says "close to brtt" but gives no numeric tolerance. Seeded 1.1
 * (72090). Sweep; too tight and FI never engages, too loose and it fires under real
 * queueing.
 */
#define SMARTT_REPS_FAST_INC_THRESH_FXP16  (72090)  /* 1.1x -- FIXME guess (§3.4 "close to") */

/* ===== Wait-to-Decrease + ECN-hold (paper §3.6.1, signal mapping) ===== */

/*
 * wtd_alpha: EWMA weight for avg_wtd (FXP16). Paper §3.6.1:
 *   avg_wtd = alpha*(ecn?1:0) + (1-alpha)*avg_wtd
 * A decrease on ECN is gated by avg_wtd >= wtd_thresh (damps reacting to isolated marks).
 *
 * FIXME: the paper never gives alpha. Seeded 0.125 (8192), the classic TCP EWMA weight.
 * Because CNPs are receiver-rate-limited (not 1:1 with marks) on this hardware, alpha/
 * thresh/hold are the compensating levers -- watch wtd_blocked vs fd/md counters.
 */
#define SMARTT_REPS_WTD_ALPHA_FXP16  (8192)  /* alpha = 0.125 -- FIXME paper gives none (§3.6.1) */

/*
 * wtd_thresh: avg_wtd gate for ECN-driven decreases (FXP16). Paper §3.6.1 value 0.25
 * (16384). 0 disables WTD gating entirely (every asserted ECN can decrease).
 */
#define SMARTT_REPS_WTD_THRESH_FXP16  (16384)  /* 0.25 (§3.6.1); 0 disables WTD */

/*
 * ecn_cnp_hold_ns: the CNP -> "ECN asserted" hold window (ns). p.ecn is true for events
 * within this window after the last CNP arrival.
 *
 * FIXME: BF3-specific -- the paper's per-packet ECN bit is unavailable; the RoCE receiver
 * emits rate-limited CNPs instead. Seeded 10000 ns (~>1 trtt). Sweep vs cnp_events and
 * queue behaviour.
 */
#define SMARTT_REPS_ECN_CNP_HOLD_NS  (10000)  /* ~>1 trtt -- FIXME CNP->ECN window (signal map) */

/* ===== cwnd bounds (paper §3.5) ===== */

/*
 * max_cwnd_frac: cwnd cap as a fraction of BDP (FXP16). cwnd is clamped to
 *   max(min_cwnd, fxp_mult(max_cwnd_frac, bdp_fxp16)).
 *
 * Paper §3.5 discusses a 1.25*BDP cap (81920); Algorithm 1 line 36 shows 1.0*BDP -- the
 * §3.5 text (headroom) wins and this is tunable. Above BDP the rate clamps at MAX anyway,
 * so this mainly bounds integrator windup.
 */
#define SMARTT_REPS_MAX_CWND_FRAC_FXP16  (81920)  /* 1.25 * BDP (§3.5; Alg1 line 36 shows 1.0) */

/*
 * min_cwnd: cwnd floor (FXP16 packets). Paper §3.5: a 1 MTU floor. 1.0 = 65536.
 * (Incast degree here is 2, so the paper's sub-1-MTU pacer option is not needed.)
 */
#define SMARTT_REPS_MIN_CWND_FXP16  (65536)  /* 1 MTU floor (§3.5) */

/*
 * init_cwnd: new-flow starting window (FXP16 packets).
 * SENTINEL 0: when init_cwnd == 0 the device starts a new flow at bdp_fxp16 (paper:
 * "the initial value of cwnd has little effect").
 */
#define SMARTT_REPS_INIT_CWND_FXP16  (0)  /* 0 => BDP (paper: init cwnd has little effect) */

/* ===== cwnd -> rate actuation (BDP identity) ===== */

/*
 * BlueField-3 has no usable congestion-window actuator (doca_pcc_dev_results_t.credits
 * is unbacked on BF3), so SMaRTT actuates its cwnd through the FXP20 rate limiter,
 * exactly like Swift:
 *   bdp_fxp16  = line_rate_mbps * base_rtt_ns * 2^16 / (mss_bytes * 8000)
 *   rate_fxp20 = clamp((cwnd_fxp16 << 20) / bdp_fxp16, min_rate, MAX_RATE)
 * cwnd == BDP -> line rate; cwnd < 1 -> deep pacing.
 */
#define SMARTT_REPS_MSS_BYTES       (4096)    /* FIXME: verify the RoCE MTU/segment size in use */
#define SMARTT_REPS_LINE_RATE_MBPS  (100000)  /* FIXME: verify NIC line rate (100 GbE assumed) */

/*
 * min_rate: rate floor in FXP20.
 * FIXME: paper defines no minimum rate. Use the same 1<<6 floor as TIMELY/DCQCN/Swift to
 * keep the rate from collapsing to zero on a long run of decreases / sub-1 cwnd.
 */
#define SMARTT_REPS_MIN_RATE_FXP20  (1 << 6)

#endif /* _SMARTT_REPS_ALGO_PARAMS_H_ */
