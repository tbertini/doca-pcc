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
 * TIMELY PPCC parameter and counter contract: the single source of truth, shared
 * by the DPA algorithm (timely.c) and the host-side dumper (timely_params_dump.c).
 *
 * Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter",
 * SIGCOMM 2015.
 *
 * Wire encodings: rates in FXP20 (1<<20 = 100% line rate), times in ns, alpha and
 * beta in FXP16 (1<<16 = 1.0), hai_threshold a raw count. Conversion to and from
 * human units is the orchestrator's job.
 */

/* TIMELY algorithm ID. */
#define TIMELY_ALGO_ID  0xBFFD

/* Parameter indices. The table is append-only: orchestrators address params by
 * index. TRACE_ON is a diagnostics toggle, not a tuning knob, and stays last. */
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
	TIMELY_P_TRACE_ON         = 9,  /* diagnostics: runtime trace toggle (0/1) */
	TIMELY_NUM_PARAMS
};

/* Counter indices. EVENTS counts every dispatched event; MD_GRADIENT, MD_THIGH, AI
 * and HAI count the Algorithm 1 branch taken; RTT_LT_TLOW and RTT_GT_THIGH classify
 * the RTT sample; RATE_CLAMPED_MIN/MAX count rate saturation at the floor and at the
 * line-rate ceiling. */
enum timely_counter_idx {
	TIMELY_C_EVENTS           = 0,
	TIMELY_C_MD_GRADIENT      = 1,
	TIMELY_C_MD_THIGH         = 2,
	TIMELY_C_AI               = 3,
	TIMELY_C_HAI              = 4,
	TIMELY_C_RTT_LT_TLOW      = 5,
	TIMELY_C_RTT_GT_THIGH     = 6,
	TIMELY_C_RATE_CLAMPED_MIN = 7,
	TIMELY_C_RATE_CLAMPED_MAX = 8,  /* rate hit the line-rate ceiling */
	TIMELY_NUM_COUNTERS
};

struct timely_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* Defaults are tuned for a 100 Gbps link, which the FXP20 values assume:
 *   N Mbps -> N*1e6 / 100e9 * (1<<20) = N*10.486 (rounded).
 *
 * alpha = 1311 (0.02 in FXP16): the paper names the EWMA weight in Algorithm 1 but
 * gives no value; 0.02 is the value recommended by the first author's published rate
 * computation snippet (https://radhikam.web.illinois.edu/timely-code-snippet.cc),
 * which also gives beta = 0.8 and hai_threshold = 5 (paper Section 6).
 */
static const struct timely_param_desc timely_params[TIMELY_NUM_PARAMS] = {
	[TIMELY_P_T_LOW_NS]        = {"t_low",         "ns",       10000,    1000,  200000},
	[TIMELY_P_T_HIGH_NS]       = {"t_high",        "ns",       80000,   10000, 2000000},
	[TIMELY_P_DELTA_FXP20]     = {"delta",         "FXP20",     1049,      10,   10486},
	[TIMELY_P_BETA_FXP16]      = {"beta",          "FXP16",    52429,   13107,   65535},
	[TIMELY_P_ALPHA_FXP16]     = {"alpha",         "FXP16",     1311,     256,   65535},
	[TIMELY_P_MIN_RTT_NS]      = {"min_rtt_ns",    "ns",        3800,     500,  100000},
	[TIMELY_P_HAI_THRESHOLD]   = {"hai_threshold", "count",        5,       1,      50},
	[TIMELY_P_MIN_RATE_FXP20]  = {"min_rate",      "FXP20",       64,       1,  104858},
	[TIMELY_P_INIT_RATE_FXP20] = {"init_rate",     "FXP20",  1048576,   10486, 1048576},
	/* Diagnostics only; min must be 0 so traces can be turned back off at runtime. */
	[TIMELY_P_TRACE_ON]        = {"trace_on",      "bool",         0,       0,       1},
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
	[TIMELY_C_RATE_CLAMPED_MAX] = "rate_clamped_max",
};

#endif /* TIMELY_PARAMS_H_ */
