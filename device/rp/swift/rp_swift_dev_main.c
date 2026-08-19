// DPA entry point

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "swift.h"

#define PCC_TRACE_FMT_EVENT (6)

/*
 * Main entry point of CC algorithm.
 * Dispatches every event to Swift (slot 0 params/counters), regardless of the
 * negotiated algo_slot. The NIC defaults to slot 15 (internal algo) when ECE
 * negotiation does not select a custom slot; per NVIDIA docs, users may run a specific
 * algorithm without considering algorithm negotiation, so we always read slot 0.
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
	/* PPCC port mapping quirk: mlxreg local_port=1 writes DPA port 0, but mlx5_0
	 * traffic arrives on DPA port 1. Pin params/counters to the port the PPCC channel
	 * targets (0), otherwise runtime tuning silently no-ops and counters land on a
	 * port the orchestrator never reads. Revisit if a build ever drives multiple ports. */
	const uint32_t SWIFT_PPCC_PORT = 0;
	uint32_t *param = doca_pcc_dev_get_algo_params(SWIFT_PPCC_PORT, 0);
	uint32_t *counter = doca_pcc_dev_get_counters(SWIFT_PPCC_PORT, 0);

	swift_algo(event, param, counter, algo_ctxt, results);

	/* Generic per-event (format 6) trace, gated by the trace_on PPCC param. arg1 is
	 * the packed tsflow (ts_ns high 32 / flow_tag low 32), matching formats 15-19. */
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
 *
 * Swift relies on RTT events produced by the NP in response to results->rtt_req=1,
 * so no event types are disabled.
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

	/* Do not mask any event types; Swift needs RTT events to fire. */
	*disable_event_bitmask = 0;
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
			/* A PPCC set targeting a slot other than 0 is a misroute: FAIL so it
			 * surfaces instead of silently no-op'ing. */
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}
	return ret;
}
