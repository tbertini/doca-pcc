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

#ifndef MULTI_MUX_PARAMS_H_
#define MULTI_MUX_PARAMS_H_

#include <stdint.h>

/*
 * Single source of truth for the multi-algo mux's PPCC parameter and counter contract.
 *
 * Included by:
 *   - rp_multi_dev_main.c (DPA): multi_mux_init() registers the param/counter from
 *     multi_mux_params[].
 *
 * The mux is a fifth algorithm slot (algo_idx/algo_slot 4, alongside DCQCN=0,
 * TIMELY=1, Swift=2, SMaRTT=3) that carries no congestion-control logic of its own.
 * Its single parameter, algo_select, picks which of the four real algorithms
 * doca_pcc_dev_user_algo() dispatches every event to, at runtime, with no rebuild.
 * See device/rp/multi/README.md for the frozen registry this header must not deviate
 * from.
 *
 * Wire encoding: algo_select is a plain integer (uint32), range [0,3].
 */

/* Multi-algo mux ID. Project convention: TIMELY=0xBFFD, DCQCN=0xBFFE, SWIFT=0xBFFC,
 * SMaRTT=0xBFFB, multi mux=0xBFFA, rtt_template/template=0xBFFF. Verified against
 * README.md's frozen registry. */
#define MULTI_MUX_ALGO_ID 0xBFFA

/*
 * Parameter indices.
 *
 * APPEND-ONLY like every other PPCC contract in this repo, even though v1 ships with
 * a single entry.
 */
typedef enum {
	/* Selects the active algorithm slot: 0=DCQCN 1=TIMELY 2=Swift 3=SMaRTT */
	MULTI_MUX_PARAM_ALGO_SELECT = 0,
	MULTI_MUX_PARAM_NUM /* Total number of configurable parameters */
} multi_mux_params_t;

/*
 * Counter indices. One placeholder counter, never written: the mux itself does no
 * per-event work to count, and the real per-event counts live in each of the four
 * algorithms' own contracts (their own slot's counters).
 */
typedef enum {
	MULTI_MUX_COUNTER_RESERVED0 = 0,
	MULTI_MUX_COUNTER_NUM
} multi_mux_counter_t;

struct multi_mux_param_desc {
	const char *name;
	const char *unit;
	uint32_t def;
	uint32_t min;
	uint32_t max;
};

/* THE contract. Edit only here. */
static const struct multi_mux_param_desc multi_mux_params[MULTI_MUX_PARAM_NUM] = {
	[MULTI_MUX_PARAM_ALGO_SELECT] = {"algo_select", "index", 0, 0, 3},
};

static const char *const multi_mux_counter_names[MULTI_MUX_COUNTER_NUM] = {
	[MULTI_MUX_COUNTER_RESERVED0] = "reserved0",
};

#endif /* MULTI_MUX_PARAMS_H_ */
