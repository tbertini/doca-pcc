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

/*
 * FIXRATE: fixed-rate stub algorithm for the slot-experiment app.
 *
 * THROWAWAY -- see device/rp/slot_exp/NOTES.md and fixrate.h. Not a congestion
 * control algorithm: exists purely so algo slot 1 has something distinguishable
 * from TIMELY (slot 0) running in it, to validate DOCA PCC's multi-algo slot
 * dispatch on real BF3 hardware.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "fixrate.h"

/* Description strings kept volatile to prevent dead-code elimination by LTO,
 * matching timely.c's / rtt_template.c's convention in this repo. */
const volatile char fixrate_desc[] = "FIXRATE v1.0 (slot-experiment stub, THROWAWAY -- see NOTES.md)";
static const volatile char fixrate_param_rate_desc[] = "RATE_FXP20, fixed TX rate handed back on every event (FXP20 of line rate)";
static const volatile char fixrate_counter_events_desc[] = "EVENTS, total events dispatched to algo slot 1";
static const volatile char fixrate_counter_flow_inits_desc[] = "FLOW_INITS, distinct flows seen on algo slot 1";

void fixrate_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = FIXRATE_ALGO_ID;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(fixrate_desc);
	algo_def.algo_desc_addr = (uint64_t)fixrate_desc;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, FIXRATE_NUM_PARAMS, FIXRATE_NUM_COUNTERS);

	/* (algo_idx, param_id, default, max, min, permissions, desc_size, desc_addr) --
	 * argument order confirmed against doca_pcc_dev_algo_access.h and timely_init()'s
	 * call site, NOT (default, min, max). */
	doca_pcc_dev_algo_init_param(algo_idx, FIXRATE_P_RATE_FXP20,
				     524288 /* default: 50% line rate */,
				     1048576 /* max */,
				     1024 /* min */,
				     1 /* permissions: writable */,
				     sizeof(fixrate_param_rate_desc), (uint64_t)fixrate_param_rate_desc);

	/* Counters registered identically to TIMELY's convention: max=UINT32_MAX,
	 * permissions=2 (matches the NVIDIA reference rtt_template). */
	doca_pcc_dev_algo_init_counter(algo_idx, FIXRATE_C_EVENTS, UINT32_MAX, 2,
				       sizeof(fixrate_counter_events_desc),
				       (uint64_t)fixrate_counter_events_desc);
	doca_pcc_dev_algo_init_counter(algo_idx, FIXRATE_C_FLOW_INITS, UINT32_MAX, 2,
				       sizeof(fixrate_counter_flow_inits_desc),
				       (uint64_t)fixrate_counter_flow_inits_desc);
}

void fixrate_algo(doca_pcc_dev_event_t *event,
		  uint32_t *param,
		  uint32_t *counter,
		  doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		  doca_pcc_dev_results_t *results)
{
	cc_ctxt_fixrate_t *fr_ctx = (cc_ctxt_fixrate_t *)algo_ctxt;

	/* fixrate answers every event identically regardless of its content --
	 * only the per-flow "have we seen this flow before" bit matters here. */
	(void)event;

	if (fr_ctx->inited == 0) {
		fr_ctx->inited = 1;
		if (counter != NULL)
			counter[FIXRATE_C_FLOW_INITS]++;
	}
	if (counter != NULL)
		counter[FIXRATE_C_EVENTS]++;

	results->rate = param[FIXRATE_P_RATE_FXP20];
	results->rtt_req = 0;
}

doca_pcc_dev_error_t fixrate_set_algo_params(uint32_t param_id_base,
					     uint32_t param_num,
					     const uint32_t *new_param_values,
					     uint32_t *params)
{
	/* Same bounds discipline as timely_set_algo_params(): reject writes that
	 * would run off the end of the (1-element) param array. */
	if ((param_id_base >= FIXRATE_NUM_PARAMS) ||
	    (param_num > FIXRATE_NUM_PARAMS) ||
	    (param_id_base + param_num > FIXRATE_NUM_PARAMS))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	for (uint32_t i = 0; i < param_num; i++)
		params[param_id_base + i] = new_param_values[i];

	return DOCA_PCC_DEV_STATUS_OK;
}
