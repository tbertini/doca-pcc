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

#ifndef _DCQCN_ALGO_PARAMS_H_
#define _DCQCN_ALGO_PARAMS_H_

/*
 * DCQCN Algorithm Default Parameters
 *
 * All values from Zhu et al. 2015, Figure 14 and Section 5.2,
 * converted to DOCA PCC fixed-point formats.
 *
 * Rate values are in FXP20 (1<<20 = 100% line rate).
 * Alpha values are in FXP16 (1<<16 = 1.0).
 * Time values are in nanoseconds.
 * Byte values are in bytes.
 */

/* Timer period T = 55 microseconds (paper Section 5.2) */
#define DCQCN_TIMER_PERIOD_NS (55000)

/* Byte counter threshold B = 10 MB (paper Section 5.2) */
#define DCQCN_BYTE_COUNTER_TH (10485760)

/*
 * g parameter stored as shift count: g = 1/(1 << G_SHIFT).
 * Paper Figure 14: g = 1/256 = 1/(1<<8), so G_SHIFT = 8.
 */
#define DCQCN_G_SHIFT (8)

/* Fast recovery threshold F = 5 (paper Section 3.1) */
#define DCQCN_F_THRESHOLD (5)

/*
 * Additive increase step R_AI = 40 Mbps, expressed as FXP20 fraction of line rate.
 * For 100 Gbps: 40e6 / 100e9 * (1<<20) = 419.43, rounded to 419
 * Paper Section 3.1: "target rate is increased in fixed steps R_AI"
 */
#define DCQCN_R_AI (419)

/*
 * Hyper additive increase step R_HAI = 5 * R_AI (from QCN standard [17]).
 * Paper Section 3.1: "There is also a hyper increase phase for fast ramp up."
 */
#define DCQCN_R_HAI (5 * DCQCN_R_AI)

/* Initial alpha = 1.0 in FXP16 (paper Section 3.1, footnote 1: "Initial value of alpha is 1") */
#define DCQCN_INITIAL_ALPHA (1 << 16)

/* New flow starting rate = 100% line rate in FXP20 (paper Section 3.1: "sends at full line rate") */
#define DCQCN_NEW_FLOW_RATE (1 << 20)

/* Minimum rate floor in FXP20 */
#define DCQCN_MIN_RATE (1 << 6)

/* Maximum rate for parameter validation (FXP20, 100% line rate) */
#define DCQCN_RATE_MAX (1 << 20)

#endif /* _DCQCN_ALGO_PARAMS_H_ */
