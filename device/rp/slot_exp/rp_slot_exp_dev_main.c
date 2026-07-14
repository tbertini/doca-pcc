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

// DPA entry point -- THROWAWAY two-algo slot-dispatch hardware experiment.
// See device/rp/slot_exp/NOTES.md for the purpose and protocol. Do not merge
// to pre-master: this app is a diagnostic harness, not a congestion control
// algorithm.
//
// Registers TIMELY (unmodified, borrowed from device/rp/timely/algo/timely.c)
// in algo slot 0 and a small fixed-rate stub (device/rp/slot_exp/algo/fixrate.c)
// in algo slot 1, and dispatches each event on attr->algo_slot via
// doca_pcc_dev_get_algo_index(), exactly like the NVIDIA reference
// rtt_template app's multi-algo skeleton (device/rp/rtt_template/rp_rtt_template_dev_main.c).

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "timely.h"
#include "fixrate.h"

#define PCC_TRACE_FMT_EVENT (6)

/*
 * Main entry point of CC algorithm.
 * This function starts the algorithm code of a single event. It receives the
 * flow context data, the event info and outputs the new rate parameters.
 *
 * Dispatches on the algo_idx currently mapped to attr->algo_slot for this
 * port (algo slot 0 -> TIMELY / algo_idx 0, algo slot 1 -> FIXRATE / algo_idx
 * 1, anything else -> the stock rtt_template internal-algo fallback, e.g. the
 * NIC's reserved slot 15 for traffic that didn't negotiate a custom algo).
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
	/* Pin PPCC param/counter access to port 0, exactly like TIMELY's standalone
	 * app (rp_timely_dev_main.c): mlxreg local_port=1 writes DPA port 0, but
	 * mlx5_0 RoCE traffic arrives on DPA port_num 1 (CLAUDE.md "BF3 PPCC port
	 * mapping quirk"). The ONLY thing that changes here vs. TIMELY's own main
	 * is the slot index -- attr->algo_slot instead of a hardcoded 0 -- so we
	 * read/increment whichever algo (TIMELY or FIXRATE) is actually mapped
	 * into the slot this event's attr says it belongs to. */
	const uint32_t SLOT_EXP_PPCC_PORT = 0;
	uint32_t algo_idx = doca_pcc_dev_get_algo_index(SLOT_EXP_PPCC_PORT, attr->algo_slot);
	uint32_t *param = doca_pcc_dev_get_algo_params(SLOT_EXP_PPCC_PORT, attr->algo_slot);
	uint32_t *counter = doca_pcc_dev_get_counters(SLOT_EXP_PPCC_PORT, attr->algo_slot);

	switch (algo_idx) {
		case 0: {
			/* TIMELY, unmodified: the exact entry point rp_timely_dev_main.c
			 * calls for every event on its (only) slot 0. */
			timely_algo(event, param, counter, algo_ctxt, results);
			break;
		}
		case 1: {
			/* FIXRATE stub -- see algo/fixrate.c. */
			fixrate_algo(event, param, counter, algo_ctxt, results);
			break;
		}
		default: {
			/* Stock rtt_template fallback (rp_rtt_template_dev_main.c) for
			 * any slot we did not register -- in particular
			 * DOCA_PCC_DEV_ALGO_SLOT_INTERNAL (0xF/15), where traffic that
			 * did not negotiate a custom algo_slot can land. */
			doca_pcc_dev_default_internal_algo(algo_ctxt, event, attr, results);
			break;
		}
	};

	/* Format 6 (shared per-event trace, see CLAUDE.md branch table) emitted
	 * UNGATED for every event -- no trace_on param check, unlike TIMELY's own
	 * main. This is a short-lived experiment binary built specifically to be
	 * read via `-l 70`, so there is no steady-state overhead concern to gate
	 * against. attr->algo_slot (NOT algo_idx) goes in the slot field: the
	 * entire point of the experiment is to see, from the trace, which slot
	 * the PCC infrastructure actually routed each event to.
	 *
	 * TSFLOW packing reused from timely.h (TIMELY_TRACE_TSFLOW) -- it is a
	 * generic ts<<32|flow_tag packer, not TIMELY-specific logic, and reusing
	 * it guarantees this trace's tsflow field decodes with the same
	 * tools/timely-prettify.sh shape as TIMELY's own format 6/11/12/14 lines. */
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

/*
 * Main entry point to user algorithm initialization.
 * This function starts the user algorithm initialization code. The function
 * will be called once per process load and should init all supported
 * algorithms and all ports.
 *
 * algo_idx 0 = TIMELY, registered by calling timely_init() -- the SAME
 * function rp_timely_dev_main.c calls -- so slot 0's metadata/params/counters
 * are byte-for-byte identical to the standalone TIMELY app.
 * algo_idx 1 = FIXRATE, registered by fixrate_init() (device/rp/slot_exp/algo/fixrate.c).
 *
 * Do not mask any event types: TIMELY (slot 0) needs RTT events to drive its
 * probe state machine, and the experiment wants to see the full event mix
 * (CNP/TX/ACK/NACK/RTT) land on both slots.
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo.
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	const uint32_t TIMELY_ALGO_IDX = 0, TIMELY_ALGO_SLOT = 0;
	const uint32_t FIXRATE_ALGO_IDX = 1, FIXRATE_ALGO_SLOT = 1;
	const uint32_t algo_en = 1;

	/* Initialize TIMELY algorithm with algo_idx=0 -- exactly as
	 * rp_timely_dev_main.c does; device/rp/timely files are not modified. */
	timely_init(TIMELY_ALGO_IDX);

	/* Initialize FIXRATE stub algorithm with algo_idx=1. */
	fixrate_init(FIXRATE_ALGO_IDX);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 uses algo_idx 0 (TIMELY), default enabled. */
		doca_pcc_dev_init_algo_slot(port_num, TIMELY_ALGO_SLOT, TIMELY_ALGO_IDX, algo_en);
		doca_pcc_dev_trace_5(0, port_num, TIMELY_ALGO_IDX, TIMELY_ALGO_SLOT, algo_en, 0);

		/* Slot 1 uses algo_idx 1 (FIXRATE), default enabled. */
		doca_pcc_dev_init_algo_slot(port_num, FIXRATE_ALGO_SLOT, FIXRATE_ALGO_IDX, algo_en);
		doca_pcc_dev_trace_5(0, port_num, FIXRATE_ALGO_IDX, FIXRATE_ALGO_SLOT, algo_en, 0);
	}

	*disable_event_bitmask = 0;
	doca_pcc_dev_printf(
		"[SLOT_EXP] %s: init complete, timely=idx%u/slot%u fixrate=idx%u/slot%u, disable_event_bitmask=0x%x\n",
		__func__,
		TIMELY_ALGO_IDX,
		TIMELY_ALGO_SLOT,
		FIXRATE_ALGO_IDX,
		FIXRATE_ALGO_SLOT,
		*disable_event_bitmask);
	doca_pcc_dev_trace_flush();
}

/*
 * Called when the parameter change was set externally.
 * The implementation should:
 *     Check the given new_parameters values. If those are correct from the algorithm perspective,
 *     assign them to the given parameter array.
 *
 * Dispatches on algo_slot: slot 0 defers to TIMELY's own handler (unmodified,
 * identical to rp_timely_dev_main.c's case), slot 1 defers to FIXRATE's
 * trivial pass-through. Any other slot is a misroute (FAIL, not silent no-op),
 * mirroring TIMELY's own convention.
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
		case 1: {
			uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

			if (algo_idx == 1)
				ret = fixrate_set_algo_params(param_id_base, param_num, new_param_values, params);
			else
				ret = DOCA_PCC_DEV_STATUS_FAIL;

			break;
		}
		default:
			/* A PPCC set targeting a slot other than 0/1 is a misroute, not a
			 * success. Return FAIL so it surfaces instead of silently no-op'ing
			 * (contract: OK means "parameters were set"). Mirrors TIMELY's own
			 * rp_timely_dev_main.c convention. */
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
	}
	return ret;
}
