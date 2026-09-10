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
 * Compile-time defaults for SMaRTT: the values a flow uses until the orchestrator pushes
 * a PPCC parameter write. They mirror the smartt_reps_params[] descriptor table in
 * smartt_reps_params.h.
 *
 * Bonato et al., "SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed & Timed
 * Transport with Recycled Entropies", arXiv:2404.01630, 2024.
 */

/* ===== Fixed-point conventions ===== */
#define SMARTT_REPS_FXP16_ONE     (1 << 16)   /* 1.0 in FXP16 (ratios, gains, cwnd packets) */
#define SMARTT_REPS_FXP16_SHIFT   (16)
#define SMARTT_REPS_RATE_MAX      (1 << 20)   /* 100% line rate in FXP20 (== DOCA_PCC_DEV_MAX_RATE) */
#define SMARTT_REPS_MTU_FXP16     (1 << 16)   /* one MTU/MSS == 1.0 packet (cwnd unit) */

/* Upper clamp on the pacing denominator: a garbage RTT sample must not stall the flow. */
#define SMARTT_REPS_PACING_RTT_MAX_NS  (100000000)  /* 100 ms */

/* ===== Delay reference (paper Section 3.5) ===== */

/* base_rtt (brtt): the no-load fabric RTT, the delay baseline the four-case loop
 * compares the measured fabric RTT against (paper Section 3.5). */
#define SMARTT_REPS_BASE_RTT_NS  (4000)  /* ns */

/* target_rtt (trtt): the delay above which the loop treats the queue as building. */
#define SMARTT_REPS_TARGET_RTT_NS  (6000)  /* ns, 1.5 * brtt (paper Section 3.5) */

/* ===== Four-case gains (paper Section 3.2) ===== */

/* fd: Fair Decrease gain, cwnd -= (cwnd/bdp) * fd * p.size (Eq. 1, Section 3.2.1). */
#define SMARTT_REPS_FD_FXP16  (52429)  /* 0.8 (paper Section 3.5) */

/* md: Multiplicative Decrease gain (Eq. 2, Section 3.2.2). Doubled from the paper's 2.0
 * because timeouts stand in for trimming here (paper Section 3.3). */
#define SMARTT_REPS_MD_FXP16  (262144)  /* 4.0 */

/* fi: Fair Increase gain, cwnd += (p.size/cwnd) * mtu * fi (Eq. 3, Section 3.2.3).
 * Held at the paper's reference-fabric value; the device scales it by gamma at use. */
#define SMARTT_REPS_FI_FXP16  (16384)  /* 0.25 (paper Section 3.5) */

/* mi: Multiplicative Increase gain (Eq. 4, Section 3.2.4), gamma-scaled on device.
 * Sentinel 0: the device derives mi = brtt/(trtt-brtt), which is 2.0 at trtt = 1.5*brtt. */
#define SMARTT_REPS_MI_FXP16  (0)  /* 0 => derive on device */

/* reference_bdp for the gamma scaling of fi and mi (paper Section 3.5 "Scaling");
 * fd and md are fixed regardless of link speed. */
#define SMARTT_REPS_REF_BDP_BYTES  (150000)  /* bytes, 100 Gbps x 12 us */

/* qa_scaling: QuickAdapt target scale, cwnd = max(acked, mtu) * qa_scaling
 * (Algorithm 2, Section 3.3). */
#define SMARTT_REPS_QA_SCALING_FXP16  (52429)  /* 0.8 (paper Section 3.5) */

/* ===== FastIncrease (paper Section 3.4, Algorithm 3) ===== */

/* fast_inc_k: MTUs added per coalesced ACK arrival while FastIncrease is active. */
#define SMARTT_REPS_FAST_INC_K_FXP16  (131072)  /* 2 MTU per ACK (paper Section 3.5) */

/* fast_inc_thresh: FastIncrease engages while rtt <= brtt * fast_inc_thresh. The paper
 * says "close to brtt" (Section 3.4) without giving a numeric tolerance. */
#define SMARTT_REPS_FAST_INC_THRESH_FXP16  (72090)  /* 1.1 */

/* ===== Wait-to-Decrease and ECN-hold (paper Section 3.6.1) ===== */

/* wtd_alpha: EWMA weight in avg_wtd = alpha*(ecn?1:0) + (1-alpha)*avg_wtd, which gates
 * ECN-driven decreases (Section 3.6.1, which gives no value for alpha). */
#define SMARTT_REPS_WTD_ALPHA_FXP16  (8192)  /* 0.125 */

/* wtd_thresh: avg_wtd gate for ECN-driven decreases; 0 disables WTD gating entirely. */
#define SMARTT_REPS_WTD_THRESH_FXP16  (16384)  /* 0.25 (paper Section 3.6.1) */

/* ecn_cnp_hold_ns: p.ecn is asserted for events within this window after the last CNP
 * arrival. The paper's per-packet ECN bit is unavailable; the RoCE receiver emits
 * rate-limited CNPs instead. */
#define SMARTT_REPS_ECN_CNP_HOLD_NS  (10000)  /* ns */

/* ===== cwnd bounds (paper Section 3.5) ===== */

/* max_cwnd_frac: cwnd cap as a fraction of BDP, clamped to
 * max(min_cwnd, fxp_mult(max_cwnd_frac, bdp_fxp16)). */
#define SMARTT_REPS_MAX_CWND_FRAC_FXP16  (81920)  /* 1.25 (Section 3.5; Algorithm 1 line 36 shows 1.0) */

/* min_cwnd: cwnd floor, one MTU (paper Section 3.5). */
#define SMARTT_REPS_MIN_CWND_FXP16  (65536)  /* 1.0 packet */

/* init_cwnd: new-flow starting window. Sentinel 0: start a new flow at bdp_fxp16. */
#define SMARTT_REPS_INIT_CWND_FXP16  (0)  /* 0 => BDP */

/* ===== cwnd -> rate actuation (BDP identity) ===== */

/*
 * BlueField-3 has no usable congestion-window actuator (doca_pcc_dev_results_t.credits
 * is unbacked), so cwnd is actuated through the FXP20 rate limiter:
 *   bdp_fxp16  = line_rate_mbps * base_rtt_ns * 2^16 / (mss_bytes * 8000)
 *   rate_fxp20 = clamp((cwnd_fxp16 << 20) / bdp_fxp16, min_rate, MAX_RATE)
 */
#define SMARTT_REPS_MSS_BYTES       (4096)    /* bytes, RoCE segment size */
#define SMARTT_REPS_LINE_RATE_MBPS  (100000)  /* Mbps, 100 GbE */

/* min_rate: rate floor, keeping the rate from collapsing to zero on a long run of
 * decreases. The paper defines no minimum rate. */
#define SMARTT_REPS_MIN_RATE_FXP20  (1 << 6)  /* FXP20 */

#endif /* _SMARTT_REPS_ALGO_PARAMS_H_ */
