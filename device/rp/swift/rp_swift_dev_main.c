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
 * DPA entry points for the Swift RP algorithm: per-event dispatch, one-time init, and
 * the PPCC parameter-set callback.
 * Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
 * Datacenter", SIGCOMM 2020.
 */

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "swift.h"

#define PCC_TRACE_FMT_EVENT (6)

#define DOCA_PCC_DEV_EVNT_ROCE_CNP_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_CNP)

/*
 * Main entry point of CC algorithm.
 * Dispatches every event to Swift and always reads slot-0 params and counters,
 * regardless of the negotiated algo_slot: the NIC defaults to slot 15 (internal algo)
 * when ECE negotiation does not select a custom slot, and this build runs Swift either
 * way.
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
	/* PPCC port mapping: mlxreg local_port=1 writes DPA port 0, but mlx5_0 traffic
	 * arrives on DPA port 1. Params and counters are pinned to the port the PPCC
	 * channel targets (0), otherwise runtime tuning silently no-ops and counters land
	 * on a port the orchestrator never reads. */
	const uint32_t SWIFT_PPCC_PORT = 0;
	uint32_t *param = doca_pcc_dev_get_algo_params(SWIFT_PPCC_PORT, 0);
	uint32_t *counter = doca_pcc_dev_get_counters(SWIFT_PPCC_PORT, 0);

	swift_algo(event, param, counter, algo_ctxt, results);

	/* Generic per-event (format 6) trace, gated by the trace_on PPCC param. arg1 is
	 * the packed tsflow: ts_ns in the high 32 bits, flow_tag in the low 32. */
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
}

/*
 * Main entry point to user algorithm initialization.
 * Called once per process load; inits the Swift algorithm and all ports.
 * Swift is driven by the RTT events produced in response to results->rtt_req=1.
 *
 * @disable_event_bitmask [out]: event types to mask out (dropped before any algo).
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	/* Initialize Swift algorithm with algo_idx=0 */
	swift_init(algo_idx);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 will use algo_idx 0 (Swift), default enabled */
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, 0);
	}

	*disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_CNP_MASK;
#ifdef SWIFT_DEBUG
	doca_pcc_dev_printf("[SWIFT] %s: init complete, disable_event_bitmask=0x%x\n",
			    __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
#endif
}

/*
 * Called when a parameter change was set externally. Validates the new values and, if
 * legal, the framework assigns them to the parameter array.
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
				ret = swift_set_algo_params(param_id_base, param_num, new_param_values, params);
			else
				ret = DOCA_PCC_DEV_STATUS_FAIL;

			break;
		}
		default:
			/* A PPCC set targeting a slot other than 0 is a misroute: fail so it
			 * surfaces instead of silently doing nothing. */
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}
	return ret;
}
