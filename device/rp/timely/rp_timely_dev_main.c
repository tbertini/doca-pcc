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
 * TIMELY DPA entry points: per-event dispatch, algorithm registration and PPCC
 * parameter writes.
 *
 * Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter",
 * SIGCOMM 2015.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "timely.h"

#define DOCA_PCC_DEV_EVNT_ROCE_CNP_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_CNP)
#define DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_ACK)
#define DOCA_PCC_DEV_EVNT_ROCE_NACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_NACK)
#define PCC_TRACE_FMT_EVENT (6)

/*
 * Main entry point of CC algorithm
 * This function starts the algorithm code of a single event.
 * It receives the flow context data, the event info and outputs the new rate parameters.
 *
 * Every event is dispatched to TIMELY using slot-0 params and counters, whatever
 * algo_slot was negotiated: the NIC falls back to slot 15 when ECE negotiation does
 * not select a custom slot.
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
	/* Param and counter arrays are per (port, slot). PPCC local_port=1 maps to DPA
	 * port_num 0, but RoCE traffic on mlx5_0 reaches the algorithm on port_num 1, so
	 * both are pinned to port 0: the port the PPCC channel actually targets. */
	const uint32_t TIMELY_PPCC_PORT = 0;
	uint32_t *param = doca_pcc_dev_get_algo_params(TIMELY_PPCC_PORT, 0);
	uint32_t *counter = doca_pcc_dev_get_counters(TIMELY_PPCC_PORT, 0);

	timely_algo(event, param, counter, algo_ctxt, results);

	/* Format 6 (per-event summary) trace, gated by the trace_on PPCC param. ev_attr is
	 * decoded inside the gate because nothing outside the trace uses it. */
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
}

/*
 * Main entry point to user algorithm initialization
 * This function starts the user algorithm initialization code.
 * The function will be called once per process load and should init all supported
 * algorithms and all ports.
 *
 * TIMELY acts on two event types only: RTT events (ev_type=DOCA_PCC_DEV_EVNT_RTT,
 * produced by the NP in response to results->rtt_req=1) drive the rate computation,
 * and TX events drive the probe watchdog plus first-event flow init.
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo.
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	timely_init(algo_idx);

	/* Enable TIMELY (algo_idx 0) in slot 0 on every port. */
	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, 0);
	}

	*disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_CNP_MASK |
				 DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK |
				 DOCA_PCC_DEV_EVNT_ROCE_NACK_MASK;
#ifdef TIMELY_DEBUG
	doca_pcc_dev_printf("[TIMELY] %s: init complete, disable_event_bitmask=0x%x\n",
			    __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
#endif
}

/*
 * Called when the parameter change was set externally.
 * The implementation should:
 *     Check the given new_parameters values. If those are correct from the algorithm perspective,
 *     assign them to the given parameter array.
 *
 * @port_num [in]: index of the port
 * @algo_slot [in]: Algo slot identifier as referred to in the PPCC command field "algo_slot"
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: pointer to an array which holds param_num number of new values for parameters
 * @params [in]: pointer to an array which holds beginning of the current parameters to be changed
 * @return -
 * DOCA_PCC_DEV_STATUS_OK: Parameters were set
 * DOCA_PCC_DEV_STATUS_FAIL: the values (one or more) are not legal. No parameters were changed
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
				ret = timely_set_algo_params(param_id_base, param_num, new_param_values, params);
			else
				ret = DOCA_PCC_DEV_STATUS_FAIL;

			break;
		}
		default:
			/* A set targeting any slot other than 0 is a misroute: OK would
			 * claim the parameters were applied. */
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}
	return ret;
}
