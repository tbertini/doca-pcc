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

#ifndef TIMELY_PARAMS_H_
#define TIMELY_PARAMS_H_

#include <stdint.h>

/*
 * Single source of truth for TIMELY's PPCC parameter and counter contract.
 *
 * Included by:
 *   - timely.c (DPA): timely_init() registers params/counters from timely_params[].
 *   - timely_params_dump.c (host): emits JSON for incast-bench's parameter encoder.
 *
 * Wire encoding for rate parameters is FXP20 (1<<20 = 100% line rate, dimensionless
 * fraction). Off-host tooling (incast-bench) is responsible for human Mbps -> FXP20
 * conversion using the configured line rate. Time parameters are nanoseconds. Alpha
 * and beta are FXP16 (1<<16 = 1.0). HAI threshold is a raw count.
 *
 * Defaults are aligned with the pre-PPCC hardcoded values in timely_algo_params.h
 * so that a doca_pcc instance running without any param writes behaves identically
 * to the pre-Phase-1 build. The orchestrator (incast-bench) overrides these at
 * runtime via PPCC cmd_type=0x8/0xB when sweeping the parameter space.
 */

/* TIMELY algorithm ID. Project convention: DCQCN=0xBFFE, rtt_template=0xBFFF. */
#define TIMELY_ALGO_ID  0xBFFD

/* Parameter indices. Order matches the existing DPA registration order; INIT_RATE
 * is appended as a new 9th parameter (was hardcoded as TIMELY_NEW_FLOW_RATE_FXP20). */
enum timely_param_idx {
	TIMELY_P_T_LOW_NS         = 0,
	TIMELY_P_T_HIGH_NS        = 1,
	TIMELY_P_DELTA_FXP20      = 2,
	TIMELY_P_BETA_FXP16       = 3,
	TIMELY_P_ALPHA_FXP16      = 4,
	TIMELY_P_MIN_RTT_NS       = 5,
	TIMELY_P_HAI_THRESHOLD    = 6,
	TIMELY_P_MIN_RATE_FXP20   = 7,
	TIMELY_P_INIT_RATE_FXP20  = 8,
	TIMELY_NUM_PARAMS
};

/* Counter indices. EVENTS is the total event count; the four branch counters
 * (MD_GRADIENT, MD_THIGH, AI, HAI) sum to ~EVENTS minus events that took none of
 * those branches (e.g., RTT events from spurious probes, TX/NACK/CNP). RTT_LT_TLOW
 * and RTT_GT_THIGH are independent classification counters (RTT_LT_TLOW counts
 * coincide with AI on the T_low branch; RTT_GT_THIGH coincide with MD_THIGH). */
enum timely_counter_idx {
	TIMELY_C_EVENTS           = 0,
	TIMELY_C_MD_GRADIENT      = 1,
	TIMELY_C_MD_THIGH         = 2,
	TIMELY_C_AI               = 3,
	TIMELY_C_HAI              = 4,
	TIMELY_C_RTT_LT_TLOW      = 5,
	TIMELY_C_RTT_GT_THIGH     = 6,
	TIMELY_C_RATE_CLAMPED_MIN = 7,
	TIMELY_NUM_COUNTERS
};

struct timely_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* THE contract. Edit only here.
 *
 * Defaults match timely_algo_params.h (pre-PPCC measured-good values on a
 * 100 Gbps BlueField-3 fabric). FXP20 values assume a 100 Gbps line rate:
 *   N Mbps -> N*1e6 / 100e9 * (1<<20) = N*10.486 (rounded).
 */
static const struct timely_param_desc timely_params[TIMELY_NUM_PARAMS] = {
	[TIMELY_P_T_LOW_NS]        = {"t_low",         "ns",       10000,    1000,  200000},
	[TIMELY_P_T_HIGH_NS]       = {"t_high",        "ns",       80000,   10000, 2000000},
	[TIMELY_P_DELTA_FXP20]     = {"delta",         "FXP20",     1049,      10,   10486},
	[TIMELY_P_BETA_FXP16]      = {"beta",          "FXP16",    52429,   13107,   65535},
	[TIMELY_P_ALPHA_FXP16]     = {"alpha",         "FXP16",    57344,     256,   65535},
	[TIMELY_P_MIN_RTT_NS]      = {"min_rtt_ns",    "ns",        3800,     500,  100000},
	[TIMELY_P_HAI_THRESHOLD]   = {"hai_threshold", "count",        5,       1,      50},
	[TIMELY_P_MIN_RATE_FXP20]  = {"min_rate",      "FXP20",       64,       1,  104858},
	[TIMELY_P_INIT_RATE_FXP20] = {"init_rate",     "FXP20",  1048576,   10486, 1048576},
};

static const char *const timely_counter_names[TIMELY_NUM_COUNTERS] = {
	[TIMELY_C_EVENTS]           = "events",
	[TIMELY_C_MD_GRADIENT]      = "md_gradient",
	[TIMELY_C_MD_THIGH]         = "md_thigh",
	[TIMELY_C_AI]               = "ai",
	[TIMELY_C_HAI]              = "hai",
	[TIMELY_C_RTT_LT_TLOW]      = "rtt_lt_tlow",
	[TIMELY_C_RTT_GT_THIGH]     = "rtt_gt_thigh",
	[TIMELY_C_RATE_CLAMPED_MIN] = "rate_clamped_min",
};

#endif /* TIMELY_PARAMS_H_ */
