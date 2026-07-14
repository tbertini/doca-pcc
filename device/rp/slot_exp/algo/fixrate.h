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

#ifndef FIXRATE_H
#define FIXRATE_H

#include <stdint.h>

/*
 * FIXRATE: fixed-rate stub algorithm for the slot-experiment app (device/rp/slot_exp/).
 *
 * THROWAWAY -- see device/rp/slot_exp/NOTES.md. This is NOT a congestion control
 * algorithm: every event is answered with the same operator-set rate
 * (param[FIXRATE_P_RATE_FXP20]), and no RTT probes are ever requested. Its only
 * purpose is to be a second, trivially-distinguishable algo_idx/algo_id running in
 * algo slot 1 alongside TIMELY in slot 0, so the multi-algo slot machinery
 * (registration, dispatch on algo_slot, PPCC enable/disable) can be exercised on
 * real BF3 hardware with non-ECE RoCE traffic.
 *
 * Relies on caller include order, matching timely.h / rtt_template.h in this repo:
 * any translation unit including this header must have already pulled in
 * <doca_pcc_dev.h> and <doca_pcc_dev_event.h> (for doca_pcc_dev_algo_ctxt_t,
 * doca_pcc_dev_event_t, doca_pcc_dev_results_t, doca_pcc_dev_error_t).
 */

/* FIXRATE algo ID. Project registry (see CLAUDE.md branch table): next free ID
 * below SMaRTT's 0xBFFB. THROWAWAY -- free this back up if slot-experiment is
 * ever deleted; do not let a "real" algorithm inherit it by accident. */
#define FIXRATE_ALGO_ID 0xBFFA

/* Parameter indices -- the PPCC contract for slot 1. Single param: the fixed
 * rate to hand back on every event, wire-encoded FXP20 (1<<20 = 100% line rate),
 * matching TIMELY/rtt_template's rate encoding convention. */
enum fixrate_param_idx {
	FIXRATE_P_RATE_FXP20 = 0,
	FIXRATE_NUM_PARAMS
};

/* Counter indices. EVENTS is bumped on every dispatched event; FLOW_INITS is
 * bumped once per distinct flow (first event seen for that flow's algo_ctxt). */
enum fixrate_counter_idx {
	FIXRATE_C_EVENTS = 0,
	FIXRATE_C_FLOW_INITS = 1,
	FIXRATE_NUM_COUNTERS
};

/*
 * Per-flow context. Well under the BF3 48-byte cap (doca_pcc_dev_algo_ctxt_t).
 *
 * `inited` is a zero-sentinel: the framework zero-initializes a flow's ctxt the
 * first time that flow is seen, so `inited == 0` on entry means "first event for
 * this flow" -- the same convention TIMELY (cur_rate==0) and rtt_template
 * (cur_rate==0) use on their own ctxt structs.
 */
typedef struct {
	uint32_t inited;
} cc_ctxt_fixrate_t;

/*
 * Entry point to FIXRATE user algorithm initialization. Registers metadata,
 * the single param, and the two counters for algo_idx. Called once per process
 * load, mirroring timely_init()'s / rtt_template_init()'s shape.
 *
 * @algo_idx [in]: Algo identifier. To be passed on to initialization APIs
 */
void fixrate_init(uint32_t algo_idx);

/*
 * Entry point to FIXRATE per-event handling.
 * Sets results->rate to the fixed param[FIXRATE_P_RATE_FXP20], never requests
 * an RTT probe (results->rtt_req = 0), bumps counter[FIXRATE_C_EVENTS] on every
 * call, and bumps counter[FIXRATE_C_FLOW_INITS] once per new flow.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions (unused: fixrate ignores event content)
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior (see PPCC access register)
 * @counter [in/out]: A pointer to an array of counters that are incremented by algo (see PPCC access register)
 * @algo_ctxt [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void fixrate_algo(doca_pcc_dev_event_t *event,
		  uint32_t *param,
		  uint32_t *counter,
		  doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		  doca_pcc_dev_results_t *results);

/*
 * Entry point to FIXRATE param writes. Bounds-checks param_id_base/param_num
 * against the array and commits; per-field min/max is already enforced by the
 * PPCC infrastructure using the min/max registered in fixrate_init() (mirrors
 * timely_set_algo_params()'s documented behavior exactly).
 *
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: pointer to an array which holds param_num number of new values for parameters
 * @params [in]: pointer to an array which holds beginning of the current parameters to be changed
 * @return: DOCA_PCC_DEV_STATUS_FAIL if input parameters (one or more) are not legal.
 */
doca_pcc_dev_error_t fixrate_set_algo_params(uint32_t param_id_base,
					     uint32_t param_num,
					     const uint32_t *new_param_values,
					     uint32_t *params);

#endif /* FIXRATE_H */
