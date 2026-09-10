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
 * DPA entry points for the multi-algorithm reaction-point app: one device binary
 * carrying DCQCN, TIMELY, Swift and SMaRTT plus a mux algorithm whose single
 * parameter selects, at runtime, which of the four handles each congestion event.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "dcqcn.h"
#include "timely.h"
#include "swift.h"
#include "smartt_reps.h"
#include "multi_mux_params.h"

/* Shared per-event summary trace; ID must match trace_message_formats[] in host/pcc_core.c. */
#define PCC_TRACE_FMT_EVENT (6)

/* Mux param-change trace: old_select, new_select, port, slot. */
#define PCC_TRACE_FMT_MUX (25)

static const volatile char multi_mux_desc[] = "multi-algo mux v1.0";
static const volatile char multi_mux_param_algo_select_desc[] =
	"ALGO_SELECT, active algorithm slot: 0=DCQCN 1=TIMELY 2=Swift 3=SMaRTT";
static const volatile char multi_mux_counter_reserved0_desc[] = "RESERVED0, unused placeholder counter";

/*
 * Registers the mux as an algorithm of its own: no congestion-control logic, just
 * the algo_select parameter that doca_pcc_dev_user_algo() reads on every event to
 * pick which of the four real algorithms (algo_idx 0-3) handles it.
 *
 * @algo_idx [in]: Algo identifier. To be passed on to initialization APIs.
 */
static void multi_mux_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = MULTI_MUX_ALGO_ID;
	algo_def.algo_major_version = 0x01;
	algo_def.algo_minor_version = 0x00;
	algo_def.algo_desc_size = sizeof(multi_mux_desc);
	algo_def.algo_desc_addr = (uint64_t)multi_mux_desc;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, MULTI_MUX_PARAM_NUM, MULTI_MUX_COUNTER_NUM);

	doca_pcc_dev_algo_init_param(algo_idx,
				     MULTI_MUX_PARAM_ALGO_SELECT,
				     multi_mux_params[MULTI_MUX_PARAM_ALGO_SELECT].def,
				     multi_mux_params[MULTI_MUX_PARAM_ALGO_SELECT].max,
				     multi_mux_params[MULTI_MUX_PARAM_ALGO_SELECT].min,
				     1,
				     sizeof(multi_mux_param_algo_select_desc),
				     (uint64_t)multi_mux_param_algo_select_desc);

	doca_pcc_dev_algo_init_counter(algo_idx,
				       MULTI_MUX_COUNTER_RESERVED0,
				       UINT32_MAX,
				       2,
				       sizeof(multi_mux_counter_reserved0_desc),
				       (uint64_t)multi_mux_counter_reserved0_desc);
}

/*
 * Main entry point of CC algorithm.
 *
 * Reads the mux's algo_select parameter (slot 4, port 0) and dispatches the event to
 * the selected algorithm, using that algorithm's own slot params/counters
 * (slot == algo_idx == the selector value). Params and counters are pinned to port 0
 * because mlxreg local_port=1 writes DPA port 0 while mlx5_0 traffic arrives on DPA
 * port 1. The negotiated attr->algo_slot is ignored; selection is global via the mux.
 *
 * @algo_ctxt [in]: A pointer to a flow context data retrieved by libpcc.
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @attr [in]: A pointer to additional parameters (algo type).
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			    doca_pcc_dev_event_t *event,
			    const doca_pcc_dev_attr_t *attr,
			    doca_pcc_dev_results_t *results)
{
	const uint32_t MULTI_PPCC_PORT = 0;
	uint32_t *mux_param = doca_pcc_dev_get_algo_params(MULTI_PPCC_PORT, 4);
	uint32_t sel = mux_param[MULTI_MUX_PARAM_ALGO_SELECT];
	uint32_t *param;
	uint32_t *counter;

	/* Defensive clamp; the parameter setter already rejects values above 3. */
	if (sel > 3)
		sel = 0;

	param = doca_pcc_dev_get_algo_params(MULTI_PPCC_PORT, sel);
	counter = doca_pcc_dev_get_counters(MULTI_PPCC_PORT, sel);

	switch (sel) {
	case 0: {
		dcqcn_algo(event, param, counter, algo_ctxt, results);
		if (DCQCN_TRACE_ENABLED(param)) {
			doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

			doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
					     DCQCN_TRACE_TSFLOW(doca_pcc_dev_get_timestamp(event), doca_pcc_dev_get_flowtag(event)),
					     (uint64_t)ev_attr.ev_type,
					     (uint64_t)ev_attr.port_num,
					     (uint64_t)attr->algo_slot,
					     (uint64_t)results->rate);
			doca_pcc_dev_trace_flush();
		}
		break;
	}
	case 1: {
		timely_algo(event, param, counter, algo_ctxt, results);
		if (TIMELY_TRACE_ENABLED(param)) {
			doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

			doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
					     (uint64_t)TIMELY_TRACE_TSFLOW(doca_pcc_dev_get_timestamp(event),
									   doca_pcc_dev_get_flowtag(event)),
					     (uint64_t)ev_attr.ev_type,
					     (uint64_t)ev_attr.port_num,
					     (uint64_t)attr->algo_slot,
					     (uint64_t)results->rate);
			doca_pcc_dev_trace_flush();
		}
		break;
	}
	case 2: {
		swift_algo(event, param, counter, algo_ctxt, results);
		if (SWIFT_TRACE_ENABLED(param)) {
			doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

			doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
					     (uint64_t)SWIFT_TRACE_TSFLOW(doca_pcc_dev_get_timestamp(event),
									   doca_pcc_dev_get_flowtag(event)),
					     (uint64_t)ev_attr.ev_type,
					     (uint64_t)ev_attr.port_num,
					     (uint64_t)attr->algo_slot,
					     (uint64_t)results->rate);
			doca_pcc_dev_trace_flush();
		}
		break;
	}
	case 3: {
		smartt_reps_algo(event, param, counter, algo_ctxt, results);
		if (SMARTT_REPS_TRACE_ENABLED(param)) {
			doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

			doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
					     SMARTT_REPS_TRACE_TSFLOW(doca_pcc_dev_get_timestamp(event),
								      doca_pcc_dev_get_flowtag(event)),
					     (uint64_t)ev_attr.ev_type,
					     (uint64_t)ev_attr.port_num,
					     (uint64_t)attr->algo_slot,
					     (uint64_t)results->rate);
			doca_pcc_dev_trace_flush();
		}
		break;
	}
	default:
		/* Unreachable; falls back to DCQCN rather than leaving *results unset. */
		dcqcn_algo(event, param, counter, algo_ctxt, results);
		break;
	}
}

/*
 * Main entry point to user algorithm initialization.
 * Called once per process load; registers all four algorithms plus the mux, and
 * enables all five slots on every port. Each algorithm keeps the algo_idx it uses in
 * its single-algo app (DCQCN=0, TIMELY=1, Swift=2, SMaRTT=3); the mux is algo_idx 4.
 *
 * No event type is masked: SMaRTT drives its control loop from ACK events, and the
 * other three pass unused event types through with cur_rate and rtt_req=0.
 *
 * @disable_event_bitmask [out]: event types to mask out (dropped before any algo).
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_en = 1;

	dcqcn_init(0);
	timely_init(1);
	swift_init(2);
	smartt_reps_init(3);
	multi_mux_init(4);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		for (uint32_t algo_slot = 0; algo_slot <= 4; ++algo_slot) {
			doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_slot, algo_en);
			doca_pcc_dev_trace_5(0, port_num, algo_slot, algo_slot, algo_en, 0);
		}
	}

	*disable_event_bitmask = 0;
	doca_pcc_dev_printf("[MULTI] %s: init complete, disable_event_bitmask=0x%x\n",
			    __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
}

/*
 * Called when a parameter change was set externally. Validates the new values and,
 * if legal, assigns them to the parameter array.
 *
 * Slots 0-3 fetch the live algo_idx for the slot, confirm it still matches the slot
 * number, and dispatch to that algorithm's own setter. Slot 4 is the mux: it
 * bounds-checks param_id_base/param_num against its one-entry contract, rejects any
 * algo_select value above 3, traces the change, then writes the validated values
 * into params. The framework does not copy new_param_values; the callback must.
 *
 * @port_num [in]: index of the port
 * @algo_slot [in]: Algo slot identifier as referred to in the PPCC command field "algo_slot"
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: array of param_num new values
 * @params [in]: array holding the current parameters to be changed
 * @return: DOCA_PCC_DEV_STATUS_OK if set; DOCA_PCC_DEV_STATUS_FAIL if values are illegal.
 */
doca_pcc_dev_error_t doca_pcc_dev_user_set_algo_params(uint32_t port_num,
						       uint32_t algo_slot,
						       uint32_t param_id_base,
						       uint32_t param_num,
						       const uint32_t *new_param_values,
						       uint32_t *params)
{
	doca_pcc_dev_error_t ret = DOCA_PCC_DEV_STATUS_OK;

	switch (algo_slot) {
	case 0: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 0)
			ret = dcqcn_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	case 1: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 1)
			ret = timely_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	case 2: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 2)
			ret = swift_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	case 3: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 3)
			ret = smartt_reps_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	case 4: {
		uint32_t old_value;
		uint32_t i;

		if ((param_id_base >= MULTI_MUX_PARAM_NUM) ||
		    (param_num > MULTI_MUX_PARAM_NUM) ||
		    (param_id_base + param_num > MULTI_MUX_PARAM_NUM)) {
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}
		if ((new_param_values == NULL) || (params == NULL)) {
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}

		for (i = 0; i < param_num; i++) {
			if (new_param_values[i] > 3) {
				ret = DOCA_PCC_DEV_STATUS_FAIL;
				break;
			}
		}
		if (ret != DOCA_PCC_DEV_STATUS_OK)
			break;

		old_value = params[MULTI_MUX_PARAM_ALGO_SELECT];
		if (param_num > 0) {
			doca_pcc_dev_trace_5(PCC_TRACE_FMT_MUX,
					     (uint64_t)old_value,
					     (uint64_t)new_param_values[0],
					     (uint64_t)port_num,
					     (uint64_t)algo_slot,
					     (uint64_t)0);
			doca_pcc_dev_trace_flush();
		}

		for (i = 0; i < param_num; i++)
			params[param_id_base + i] = new_param_values[i];
		break;
	}
	default:
		ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	return ret;
}
