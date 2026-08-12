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
 * Compile-time defaults for Swift. These mirror the swift_params[] descriptor table
 * (swift_params.h) and are the values a flow uses until the orchestrator pushes a
 * PPCC param write. Each value cites a paper section or is flagged FIXME.
 *
 * Reference: Kumar et al., "Swift: Delay is Simple and Effective for Congestion
 * Control in the Datacenter", SIGCOMM 2020 (DOI 10.1145/3387514.3406591).
 *
 * NB: the paper gives formula structure and parameter names (Table 2, §3.5) but no
 * canonical numeric tuning table -- treat all tuning values as seeds to sweep.
 */

/* ===== Fixed-point conventions (match TIMELY/DCQCN) ===== */
#define SWIFT_FXP16_ONE     (1 << 16)   /* 1.0 in FXP16 (ratios, gains, cwnd packets) */
#define SWIFT_FXP16_SHIFT   (16)
#define SWIFT_RATE_MAX      (1 << 20)   /* 100% line rate in FXP20 (== DOCA_PCC_DEV_MAX_RATE) */
#define SWIFT_CWND_ONE_FXP16 (1 << 16)  /* cwnd == 1.0 packet, the AI/pacing regime boundary */

/* ===== Target delay scaling (paper §3.5, Table 2) =====
 *   t = base_target + #hops*hbar + max(0, min(alpha/sqrt(fcwnd) + beta, fs_range))
 * with alpha/beta DERIVED on-device from fs_range/fs_min_cwnd/fs_max_cwnd.
 */

/*
 * base_target: base fabric target delay (ns).
 *
 * Paper §3.5 folds the FIXED portion of fabric delay -- propagation, NIC/switch
 * serialization, small-flow queueing, timestamp measurement error -- into base_target.
 * Per the design decision (and matching TIMELY, which subtracts NO serialization and
 * folds everything into its empirically-retuned t_low/t_high), Swift v1 does NOT
 * subtract serialization separately; base_target absorbs it.
 *
 * Paper-stated points: ~25 us (a stack-limited test), 50 us fabric (a storage test).
 * FIXME: those are Google's fabric. Idle fabric RTT on this BF3 setup measured ~3-4 us
 * (cf. TIMELY min_rtt 3800 ns). A 25 us target would sit far above no-load RTT and the
 * controller would never decrease. Start ~10 us and RETUNE from a no-load fabric_rtt
 * trace (format 16). FIXME: also revisit if variable segment sizes appear -- a
 * per-segment serialization subtraction (delay -= seg_bytes*8*1e9/line_rate) would
 * then be warranted, but BF3 exposes no clean per-event segment-byte source.
 */
#define SWIFT_BASE_TARGET_NS  (10000)

/*
 * fs_range: maximum additional target delay granted to small flows (ns).
 *
 * Paper §3.5 "Flow-based Scaling": the target gains headroom as cwnd shrinks so that
 * many small flows (incast) tolerate more queueing without over-reacting. fs_range is
 * the full span of that headroom (the vertical extent in paper Figure 5).
 *
 * FIXME: paper gives no numeric value; this is topology/fabric dependent. Seeded at
 * 1.5x base_target. Sweep on hardware against incast fairness.
 */
#define SWIFT_FS_RANGE_NS  (15000)

/*
 * fs_min_cwnd / fs_max_cwnd: the two knees of the flow-scaling curve (FXP16 packets).
 *   cwnd <= fs_min_cwnd  -> fs_term = fs_range (max headroom; deep-incast small flows)
 *   cwnd >= fs_max_cwnd  -> fs_term = 0        (single flow can fill the pipe)
 *
 * Paper Figure 5 places the knees roughly at ~0.1 pkt and the upper-cwnd regime.
 * FIXME: paper gives no numeric values. Seeded 0.5 pkt and 16 pkt (the latter near a
 * ~12-46 pkt BDP for this 100G/3.8us fabric). MUST satisfy fs_min_cwnd < fs_max_cwnd
 * (swift.c guards the derived alpha against a non-positive denominator).
 */
#define SWIFT_FS_MIN_CWND_FXP16  (32768)    /* 0.5 pkt */
#define SWIFT_FS_MAX_CWND_FXP16  (1048576)  /* 16 pkt  */

/*
 * Per-hop target term: target += num_hops * hop_scale (paper §3.5 "Topology-based
 * Scaling", the #hops*hbar term).
 *
 * FIXME: DOCA mapping -- doca_pcc_dev_get_ttl_hoplimit() returns 0 on BlueField-3
 * (real only on CX8+), so #hops cannot be read from the IP TTL. num_hops is therefore
 * a FIXED configurable param and hop_scale defaults to 0, making the per-hop term
 * inert on BF3 (target = base_target + fs_term). Set hop_scale > 0 only on a fabric
 * where the per-hop latency is known and worth modelling.
 */
#define SWIFT_NUM_HOPS      (1)
#define SWIFT_HOP_SCALE_NS  (0)

/* ===== AIMD on cwnd (paper Algorithm 1) ===== */

/*
 * ai: additive increment (FXP16 packets).
 *
 * Paper Algorithm 1: on delay < target, cwnd += (ai/cwnd)*num_acked (cwnd>=1) so the
 * cumulative increase over one RTT is ai; community-typical ai = 1 packet.
 *
 * FIXME: paper does not give an authoritative ai value (1 is typical). FIXME: DOCA
 * mapping -- DOCA delivers discrete RTT-probe events (~ms apart, >> 1 RTT) with no
 * num_acked. Matching TIMELY's validated cadence on this exact fabric (timely.c:
 * "probe inter-arrival ~16 ms >> minRTT -> no scaling needed"), Swift applies ONE
 * +ai step per RTT event rather than scaling by elapsed/RTT (which would multiply ai
 * by ~1000s and explode cwnd). Tune convergence via this param / the probe cadence.
 */
#define SWIFT_AI_FXP16  (65536)  /* 1.0 pkt */

/*
 * md_beta: multiplicative-decrease gain (FXP16). Paper Algorithm 1 line 14:
 *   cwnd <- max(1 - beta*(delay-target)/delay, 1 - max_mdf) * cwnd
 *
 * FIXME: paper does not give an authoritative beta (community-typical ~0.8). 0.8 in
 * FXP16 = 52429 (matches the TIMELY beta default).
 */
#define SWIFT_MD_BETA_FXP16  (52429)  /* 0.8 */

/*
 * max_mdf: maximum multiplicative decrease factor (FXP16) -- the floor on the MD
 * multiplier, i.e. cwnd is never cut below (1 - max_mdf)*cwnd in a single step. Also
 * the cut applied on loss (paper §3.6: NACK/RTO multiply cwnd by (1 - max_mdf)).
 *
 * FIXME: paper §3.2 names max_mdf but gives no value (community-typical 0.5). 0.5 in
 * FXP16 = 32768.
 */
#define SWIFT_MAX_MDF_FXP16  (32768)  /* 0.5 */

/*
 * min_cwnd / max_cwnd / init_cwnd (FXP16 packets).
 *
 * min_cwnd: paper §3.4 explicitly augments Swift to let cwnd fall to 0.001 pkt for
 *   large-scale incast (translated to inter-packet pacing). 0.001 * 2^16 = 65.5 -> 66.
 *   NB (brief §4): the FXP20 rate floor (min_rate) must be able to represent the rate
 *   that min_cwnd maps to, else that floor -- not 0.001 pkt -- is the effective limit.
 * max_cwnd: paper §3.2 caps cwnd as an upper guard. FIXME: paper gives no value; near/
 *   above BDP (~12-46 pkt here). Seeded 64 pkt. Above BDP, rate clamps at MAX anyway,
 *   so this mainly bounds integrator windup.
 * init_cwnd: paper §3.2 "initial value of cwnd has little effect". Seeded ~16 pkt
 *   (~BDP), so a new flow starts near line rate (cf. TIMELY/DCQCN start at line rate).
 */
#define SWIFT_MIN_CWND_FXP16   (66)       /* 0.001 pkt (paper §3.4) */
#define SWIFT_MAX_CWND_FXP16   (4194304)  /* 64 pkt; FIXME near BDP */
#define SWIFT_INIT_CWND_FXP16  (1048576)  /* 16 pkt; FIXME ~BDP -> near line-rate start */

/*
 * retx_reset_threshold: consecutive NACK/RTO events after which cwnd collapses to
 * min_cwnd (paper §3.6 / Algorithm 1 "On Retransmit Timeout": if retransmit_cnt >=
 * RETX_RESET_THRESHOLD then cwnd <- min_cwnd).
 *
 * FIXME: paper does not give a numeric threshold. Seeded 3.
 */
#define SWIFT_RETX_RESET_THRESHOLD  (3)

/* ===== cwnd -> rate actuation (pacing law / BDP identity) ===== */

/*
 * BlueField-3 has no usable congestion-window actuator (doca_pcc_dev_results_t.credits
 * is unbacked on BF3), so Swift actuates its cwnd through the FXP20 rate limiter:
 *   BDP_pkts(rtt) = line_rate_bps * rtt_s / (mss_bytes * 8)
 *   rate_fxp20    = clamp((cwnd_fxp16 << 20) / bdp_fxp16(rtt), min_rate, MAX_RATE)
 * With pacing_rtt_on=1 (default), rtt = the last measured fabric RTT clamped to
 * >= min_rtt: this is the paper's pacing law rate = cwnd/rtt (Algorithm 1 lines
 * 29-32), and by Little's law it holds inflight ~= rate * rtt = cwnd as the fabric
 * loads -- the self-adjusting property of a real window. With pacing_rtt_on=0, rtt
 * = the constant min_rtt param (the pre-refinement BDP mapping, kept for A/B runs);
 * under load that admits inflight ~= cwnd * rtt_live/min_rtt (~4x at the RTTs
 * measured on this fabric: ~15 us loaded vs 3.8 us idle).
 * cwnd == BDP -> line rate; cwnd == BDP/2 -> half; cwnd < 1 -> deep pacing, which is
 * exactly Algorithm 1's pacing_delay = rtt/cwnd regime realized by the rate limiter.
 */
#define SWIFT_MSS_BYTES       (4096)    /* FIXME: verify the RoCE MTU/segment size in use */
#define SWIFT_MIN_RTT_NS      (3800)    /* measured BF3<->BF3 base RTT (cf. TIMELY) */
#define SWIFT_LINE_RATE_MBPS  (100000)  /* FIXME: verify NIC line rate (100 GbE assumed) */
#define SWIFT_PACING_RTT_ON   (1)       /* divide cwnd by live fabric RTT (paper pacing law) */

/*
 * Arithmetic guard on the pacing denominator: line_rate_mbps * rtt_ns * 2^16 must fit
 * in uint64 (worst case 800000 Mbps * rtt * 65536 -> rtt <= ~350 ms). 100 ms is far
 * beyond any real fabric RTT and comfortably inside the bound; a sample above it is a
 * broken measurement, not a signal.
 */
#define SWIFT_PACING_RTT_MAX_NS  (100000000)  /* 100 ms */

/*
 * min_rate: rate floor in FXP20.
 * FIXME: paper defines no minimum rate. Use the same 1<<6 floor as TIMELY/DCQCN to keep
 * the rate from collapsing to zero on a long run of decreases / sub-1 cwnd.
 */
#define SWIFT_MIN_RATE_FXP20  (1 << 6)

/* ===== Endpoint window (paper §3.3) -- reserved + inert in v1 (fabric-only) ===== */

/*
 * FIXME: paper §3.2/§3.3 -- the endpoint congestion window (ecwnd) is NOT implemented
 * in v1. Cleanly separating host-queue delay from NP processing on BF3 is unverified
 * (doca_pcc_dev_get_rtt_np_rx_byte_counter returns 0 on BF3; the endpoint signal would
 * need a custom NP probe-response payload via doca_pcc_dev_get_rtt_raw_data). These two
 * params are registered now -- so the param index map is stable when ecwnd lands in
 * phase 2 -- but are unused by the v1 algorithm. Paper storage-test point: ~100 us
 * endpoint target.
 */
#define SWIFT_ENDPOINT_TARGET_NS    (100000)
#define SWIFT_ENDPOINT_EWMA_FXP16   (32768)  /* 0.5 */

#endif /* _SWIFT_ALGO_PARAMS_H_ */
