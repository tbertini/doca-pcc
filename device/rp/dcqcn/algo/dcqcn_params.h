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

#ifndef DCQCN_PARAMS_H_
#define DCQCN_PARAMS_H_

#include <stdint.h>

/*
 * Single source of truth for DCQCN's PPCC parameter and counter contract.
 *
 * Included by:
 *   - dcqcn.c (DPA): dcqcn_init() registers params/counters from dcqcn_params[].
 *   - dcqcn_params_dump.c (host): emits JSON for incast-bench's parameter encoder.
 *
 * DCQCN (Zhu et al., "Congestion Control for Large-Scale RDMA Deployments", SIGCOMM
 * 2015) is a rate-based controller driven by ECN / CNPs (Congestion Notification
 * Packets), implementing the Figure 7 state machine: CutRate on CNP, plus a
 * three-phase rate increase (Fast Recovery / Additive Increase / Hyper Increase)
 * gated by a software timer T and byte counter BC.
 *
 * Wire encodings:
 *   - time    -> nanoseconds (uint32): timer_period
 *   - bytes   -> bytes (uint32): byte_counter_th
 *   - shift   -> shift count (uint32): g_shift (g = 1/(1<<g_shift))
 *   - count   -> plain integer (uint32): f_threshold
 *   - rate    -> FXP20 (1<<20 = 100% line rate): r_ai, r_hai, new_flow_rate, min_rate
 * Human-unit to FXP conversion (e.g. Mbps -> FXP20) is the orchestrator's job.
 *
 * Defaults match the compile-time defines in dcqcn_algo_params.h (Zhu et al. Figure
 * 14 / Section 5.2), so an instance running with no param writes behaves identically
 * to a build with the contract bypassed.
 *
 * TRACE_ON (index 8) is a diagnostics-only toggle, not a tuning knob: it gates
 * runtime emission of the format 6-10 device traces and rides in the param array so
 * it can be flipped over the PPCC channel without rebuilding.
 */

/* DCQCN algorithm ID, as registered by dcqcn_init(). */
#define DCQCN_ALGO_ID 0xBFFE

/*
 * Parameter indices.
 *
 * APPEND-ONLY: external orchestrators address parameters by index in bulk-set, so
 * never reorder or insert. TRACE_ON is kept LAST so the tuning indices stay stable
 * across versions.
 */
typedef enum {
	DCQCN_PARAM_TIMER_PERIOD = 0,	  /* Timer period T in nanoseconds */
	DCQCN_PARAM_BYTE_COUNTER_TH = 1, /* Byte counter threshold B in bytes */
	DCQCN_PARAM_G_SHIFT = 2,	  /* g = 1/(1 << G_SHIFT) */
	DCQCN_PARAM_F_THRESHOLD = 3,	  /* Fast recovery threshold F */
	DCQCN_PARAM_R_AI = 4,		  /* Additive increase step R_AI (FXP20) */
	DCQCN_PARAM_R_HAI = 5,		  /* Hyper additive increase step R_HAI (FXP20) */
	DCQCN_PARAM_NEW_FLOW_RATE = 6,	  /* New flow initial rate (FXP20) */
	DCQCN_PARAM_MIN_RATE = 7,	  /* Minimum rate floor (FXP20) */
	DCQCN_PARAM_TRACE_ON = 8,	  /* Runtime trace toggle (0/1); not a tuning param (KEEP LAST) */
	DCQCN_PARAM_NUM		  /* Total number of configurable parameters */
} dcqcn_params_t;

/*
 * Counter indices.
 */
typedef enum {
	DCQCN_COUNTER_TX_EVENT = 0,
	DCQCN_COUNTER_CNP_EVENT = 1,
	DCQCN_COUNTER_NACK_EVENT = 2,
	DCQCN_COUNTER_NUM
} dcqcn_counter_t;

struct dcqcn_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* The contract: name, unit, default, min and max of every parameter. Edit only here.
 * g_shift's max of 16 is a correctness bound: dcqcn_alpha_update_cnp() computes
 * 1 << (16 - g_shift), which is undefined behavior for g_shift > 16.
 */
static const struct dcqcn_param_desc dcqcn_params[DCQCN_PARAM_NUM] = {
	[DCQCN_PARAM_TIMER_PERIOD]    = {"timer_period",    "ns",     55000,       1, UINT32_MAX},
	[DCQCN_PARAM_BYTE_COUNTER_TH] = {"byte_counter_th", "bytes",  10485760,    1, UINT32_MAX},
	[DCQCN_PARAM_G_SHIFT]         = {"g_shift",         "shift",  8,           1,         16},
	[DCQCN_PARAM_F_THRESHOLD]     = {"f_threshold",     "count",  5,           1,        100},
	[DCQCN_PARAM_R_AI]            = {"r_ai",            "FXP20",  419,         1,    1048576},
	[DCQCN_PARAM_R_HAI]           = {"r_hai",           "FXP20",  2095,        1,    1048576},
	[DCQCN_PARAM_NEW_FLOW_RATE]   = {"new_flow_rate",   "FXP20",  1048576,     1,    1048576},
	[DCQCN_PARAM_MIN_RATE]        = {"min_rate",        "FXP20",  64,          1,    1048576},
	/* Diagnostics-only: gates runtime device-trace emission. min MUST be 0 so traces
	 * can be turned back off at runtime. */
	[DCQCN_PARAM_TRACE_ON]        = {"trace_on",        "bool",   0,           0,          1},
};

static const char *const dcqcn_counter_names[DCQCN_COUNTER_NUM] = {
	[DCQCN_COUNTER_TX_EVENT]   = "tx_events",
	[DCQCN_COUNTER_CNP_EVENT]  = "cnp_events",
	[DCQCN_COUNTER_NACK_EVENT] = "nack_events",
};

#endif /* DCQCN_PARAMS_H_ */
