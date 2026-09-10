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

#ifndef _SWIFT_ALGO_PARAMS_H_
#define _SWIFT_ALGO_PARAMS_H_

/*
 * Compile-time defaults for Swift.
 * Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
 * Datacenter", SIGCOMM 2020.
 * These mirror the swift_params[] descriptor table and apply until the orchestrator
 * pushes a PPCC param write.
 */

/* ===== Fixed-point conventions ===== */
#define SWIFT_FXP16_ONE     (1 << 16)   /* 1.0 in FXP16 (ratios, gains, cwnd packets) */
#define SWIFT_FXP16_SHIFT   (16)
#define SWIFT_RATE_MAX      (1 << 20)   /* 100% line rate in FXP20 (== DOCA_PCC_DEV_MAX_RATE) */
#define SWIFT_CWND_ONE_FXP16 (1 << 16)  /* cwnd == 1.0 packet, the AI/pacing regime boundary */

/* ===== Target delay scaling (paper Section 3.5) =====
 *   t = base_target + num_hops*hop_scale + max(0, min(alpha/sqrt(fcwnd) + beta, fs_range))
 * alpha/beta derived on-device from fs_range / fs_min_cwnd / fs_max_cwnd. */

/*
 * base_target: base fabric target delay (ns). It absorbs the fixed portion of fabric
 * delay (propagation, serialization); nothing is subtracted separately.
 */
#define SWIFT_BASE_TARGET_NS  (10000)

/*
 * fs_range: maximum extra target delay granted to small flows (ns), the full span of
 * the flow-scaling curve (paper Section 3.5).
 */
#define SWIFT_FS_RANGE_NS  (15000)

/*
 * fs_min_cwnd / fs_max_cwnd: knees of the flow-scaling curve (FXP16 packets):
 * fs_term = fs_range at/below fs_min_cwnd, 0 at/above fs_max_cwnd. Must satisfy
 * fs_min_cwnd < fs_max_cwnd.
 */
#define SWIFT_FS_MIN_CWND_FXP16  (32768)    /* 0.5 pkt */
#define SWIFT_FS_MAX_CWND_FXP16  (1048576)  /* 16 pkt  */

/*
 * Per-hop target term: target += num_hops * hop_scale (paper Section 3.5).
 * doca_pcc_dev_get_ttl_hoplimit() returns 0 on BF3, so num_hops is a fixed param;
 * hop_scale defaults to 0, keeping the term inert.
 */
#define SWIFT_NUM_HOPS      (1)
#define SWIFT_HOP_SCALE_NS  (0)

/* ===== AIMD on cwnd (paper Algorithm 1) ===== */

/*
 * ai: additive increment (FXP16 packets), applied once per RTT event.
 * 1 pkt, from the Bolt artifact's Swift baseline (Arslan et al., NSDI 2023).
 */
#define SWIFT_AI_FXP16  (65536)

/*
 * md_beta: multiplicative-decrease gain (FXP16), paper Algorithm 1 line 14.
 * 0.8, from the Bolt artifact's Swift baseline (Arslan et al., NSDI 2023).
 */
#define SWIFT_MD_BETA_FXP16  (52429)

/*
 * max_mdf: floor on the MD multiplier (FXP16); also the cut applied on loss
 * (paper Section 3.6).
 * 0.5, from the Bolt artifact's Swift baseline (Arslan et al., NSDI 2023).
 */
#define SWIFT_MAX_MDF_FXP16  (32768)

/*
 * min_cwnd / max_cwnd / init_cwnd (FXP16 packets).
 * min_cwnd = 0.001 pkt (paper Section 3.4). It is only reachable if min_rate can
 * represent the rate it maps to, otherwise min_rate is the effective floor.
 */
#define SWIFT_MIN_CWND_FXP16   (66)       /* 0.001 pkt */
#define SWIFT_MAX_CWND_FXP16   (4194304)  /* 64 pkt */
#define SWIFT_INIT_CWND_FXP16  (1048576)  /* 16 pkt */

/*
 * retx_reset_threshold: consecutive NACK/RTO events after which cwnd collapses to
 * min_cwnd (paper Section 3.6).
 */
#define SWIFT_RETX_RESET_THRESHOLD  (3)

/* ===== cwnd -> rate actuation ===== */

/*
 * Credits are unbacked on BF3, so cwnd is enforced through the rate limiter using the
 * paper's pacing law rate = cwnd / BDP(rtt), with rtt = the last fabric sample clamped
 * to >= min_rtt.
 */
#define SWIFT_MSS_BYTES       (4096)    /* RoCE MTU (bytes) */
#define SWIFT_MIN_RTT_NS      (3800)    /* base fabric RTT (ns) */
#define SWIFT_LINE_RATE_MBPS  (100000)  /* link rate (Mbps, 100 GbE) */

/*
 * Guard on the pacing arithmetic: line_rate_mbps * rtt_ns * 2^16 must fit in uint64;
 * a sample above 100 ms is a broken measurement, not a signal.
 */
#define SWIFT_PACING_RTT_MAX_NS  (100000000)  /* 100 ms */

/* min_rate: rate floor (FXP20). */
#define SWIFT_MIN_RATE_FXP20  (1 << 6)

/* ===== Endpoint window (paper Section 3.3) ===== */

/*
 * BF3 exposes no endpoint-delay signal, so the endpoint window (ecwnd) is inert. The two
 * params stay registered to keep the append-only index map stable; the algorithm does
 * not read them.
 */
#define SWIFT_ENDPOINT_TARGET_NS    (100000)
#define SWIFT_ENDPOINT_EWMA_FXP16   (32768)  /* 0.5 */

#endif /* _SWIFT_ALGO_PARAMS_H_ */
