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

/*
 * TIMELY Algorithm Default Parameters
 *
 * All values from Mittal et al. 2015 (SIGCOMM), Section 6 ("Unless mentioned otherwise..."),
 * converted to DOCA PCC fixed-point formats.
 *
 * Rate values are in FXP20 (1<<20 = 100% line rate).
 * EWMA weight (alpha) and decrement factor (beta) are in FXP16 (1<<16 = 1.0).
 * Time values are in nanoseconds.
 */

/* T_low = 50 microseconds (paper §6: "T_low of 50 us") */
#define TIMELY_T_LOW_NS (50000)

/* T_high = 500 microseconds (paper §6: "T_high of 500 us") */
#define TIMELY_T_HIGH_NS (500000)

/*
 * Additive increment delta = 10 Mbps (paper §6: "additive increment of 10 Mbps").
 *
 * Expressed as FXP20 fraction of 100 Gbps line rate:
 *   10e6 / 100e9 * (1<<20) = 104.857... ≈ 105
 *
 * FIXME: paper §6 specifies delta as an absolute bandwidth (10 Mbps), not as a
 * fraction of line rate. The FXP20 representation is line-rate-relative, so the
 * effective delta in absolute terms scales with the NIC's actual line rate.
 * On a 100 Gbps NIC this is exactly 10 Mbps; on 200 Gbps it would be 20 Mbps.
 * To preserve absolute semantics, this value should be retuned per NIC speed.
 */
#define TIMELY_DELTA_FXP20 (105)

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
 * FIXME: paper §4.3 says "we use a fixed value representing the wire propagation
 * delay across the datacenter network, which is known ahead of time" and notes
 * that "the exact value of the minimum RTT does not matter since we only need
 * to determine if the queue is growing or receding". However, the magnitude
 * affects the rate decrease in the gradient>0 branch:
 *     rate ← rate · (1 - β · rtt_diff/minRTT)
 * smaller minRTT → larger decrease per gradient nanosecond.
 *
 * 25 microseconds is a reasonable default for a single-hop datacenter at 100 Gbps.
 * This should be tuned to actual fabric propagation delay (measured via empty-queue RTT).
 */
#define TIMELY_MIN_RTT_NS (25000)

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
