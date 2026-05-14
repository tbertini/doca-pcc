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

#ifndef _TIMELY_ALGO_PARAMS_H_
#define _TIMELY_ALGO_PARAMS_H_


/* Paper defaults (10 Gbps fabric, ~20us base RTT):
 * #define TIMELY_T_LOW_NS  (50000)   // paper §6: "T_low of 50 us"
 * #define TIMELY_T_HIGH_NS (500000)  // paper §6: "T_high of 500 us"
 */
// Paper's values:
// #define TIMELY_T_LOW_NS  (50000)
// #define TIMELY_T_HIGH_NS (500000)

// Rate too low
// #define TIMELY_T_LOW_NS  (9500)
// #define TIMELY_T_HIGH_NS (95000)

// Rate too low
#define TIMELY_T_LOW_NS  (100000)
#define TIMELY_T_HIGH_NS (1000000)

/*
 * Additive increment delta retuned on 2026-05-03 for 100 Gbps fabric.
 *
 * Paper §6.1 used 10 Mbps on a 10 Gbps fabric, i.e. delta/line_rate = 0.001.
 * Applying the same ratio to 100 Gbps gives 100 Mbps.
 *
 * Expressed as FXP20 fraction of 100 Gbps line rate:
 *   100e6 / 100e9 * (1<<20) = 1048.576 ≈ 1049
 *
 * FIXME: the FXP20 representation is line-rate-relative, so the effective
 * delta in absolute terms scales with the NIC's actual line rate. Re-tune
 * if NIC speed changes (use device/rp/timely/timely-recommend-params.sh).
 */
/* Paper default (10 Mbps additive step, scaled to 100 Gbps line rate):
 * #define TIMELY_DELTA_FXP20 (105)   // paper §6.1
 */
#define TIMELY_DELTA_FXP20 (1049)  /* 100 Mbps additive step on 100 Gbps */

/*
 * Multiplicative decrement factor beta = 0.8 (paper §6: "multiplicative decrement factor (β) of 0.8").
 *
 * In FXP16: 0.8 * 65536 = 52428.8 ≈ 52429
 */
#define TIMELY_BETA_FXP16 (52429)

/*
 * EWMA weight parameter alpha for the rtt_diff smoothing filter.
 *
 * FIXME: paper Algorithm 1 labels alpha as "EWMA weight parameter" but does NOT
 * give a numeric value anywhere in the paper. We pick 0.875 as a default — this
 * weights recent RTT samples heavily, matching TIMELY's design goal of reacting
 * quickly to gradient changes. Common alternatives would be 0.5 or even lower
 * (more smoothing, slower reaction). This is a deviation from the paper that
 * should be evaluated empirically.
 *
 * In FXP16: 0.875 * 65536 = 57344
 */
#define TIMELY_ALPHA_FXP16 (57344)

/*
 * Minimum RTT representing wire propagation delay.
 *
 * Tuned on 2026-05-03 from a TIMELY trace on our 100 Gbps BlueField-3 fabric:
 * idle-load format-12 lines reported new_rtt clustering around 2934-2998 ns.
 * Taking 3000 ns as the wire propagation delay floor.
 *
 * Paper §4.3 says "we use a fixed value representing the wire propagation
 * delay across the datacenter network, which is known ahead of time" and notes
 * that "the exact value of the minimum RTT does not matter since we only need
 * to determine if the queue is growing or receding". However, the magnitude
 * affects the rate decrease in the gradient>0 branch:
 *     rate ← rate · (1 - β · rtt_diff/minRTT)
 * smaller minRTT → larger decrease per gradient nanosecond.
 */
/* Previous default (untuned, generic-datacenter guess):
 * #define TIMELY_MIN_RTT_NS (25000)
 */

// 3800 BF3-to-BF3 1-to-1 ib_write_bw. Paper says MIN_RTT value is not important though.
#define TIMELY_MIN_RTT_NS (3800)

/*
 * HAI (HyperActive Increase) threshold: trigger HAI mode after this many consecutive
 * completion events with gradient ≤ 0 (paper §4.3 "Hyperactive increase").
 *
 * Paper §4.3: "if TIMELY does not reach the new fair share after a period of slow
 * growth, i.e., the gradient is negative for several consecutive completion times,
 * then HAI switches to a faster additive increase".
 *
 * Paper §4.3 also states "N = 5 if gradient<0 for five completion events".
 * We interpret "several" = 5 per the paper's own example.
 */
#define TIMELY_HAI_THRESHOLD (5)

/*
 * HAI factor: multiplier applied to delta when in HAI mode.
 *
 * Paper §4.3: "the rate is incremented by Nδ instead of δ" and "N = 5".
 *
 * Hardcoded (not configurable) since the paper ties HAI_FACTOR to HAI_THRESHOLD.
 */
#define TIMELY_HAI_FACTOR (5)

/*
 * Minimum rate floor in FXP20.
 *
 * FIXME: paper does not define a minimum rate. We use the same 1<<6 floor as DCQCN
 * to prevent the rate from collapsing to zero on a long sequence of decreases.
 */
#define TIMELY_MIN_RATE_FXP20 (1 << 6)

/*
 * Initial rate for new flows = 100% line rate in FXP20.
 *
 * FIXME: paper does not explicitly specify an initial rate for TIMELY. Paper §5
 * mentions "we also cap the maximum target rate at 10 Gbps" (an absolute cap,
 * not an initial rate). We follow the common practice of starting at full line
 * rate (matching DCQCN paper §3.1: "When a flow starts, it sends at full line rate").
 */
#define TIMELY_NEW_FLOW_RATE_FXP20 (1 << 20)

/* Maximum value for rate-format parameters (used for parameter validation) */
#define TIMELY_RATE_MAX (1 << 20)

/* Maximum value for FXP16 fractions (1.0 in FXP16) */
#define TIMELY_FXP16_ONE (1 << 16)

#endif /* _TIMELY_ALGO_PARAMS_H_ */
