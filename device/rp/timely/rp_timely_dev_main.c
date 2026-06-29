// DPA entry point

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "timely.h"

#define PCC_TRACE_FMT_EVENT (6)

/*
 * Main entry point of CC algorithm
 * This function starts the algorithm code of a single event.
 * It receives the flow context data, the event info and outputs the new rate parameters.
 *
 * The function dispatches every event to TIMELY (slot 0 params/counters), regardless
 * of the negotiated algo_slot. The NIC defaults to slot 15 (internal algo) when ECE
 * negotiation doesn't select a custom slot. Per NVIDIA docs, users may run a specific
 * algorithm without considering the algorithm negotiation.
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
	/* Param/counter arrays are per (port, slot). The orchestrator pushes params and
	 * scrapes counters over mlxreg PPCC with local_port=1, which the firmware maps to
	 * DPA port_num 0 -- confirmed empirically: the set_algo_params callback fires with
	 * port_num=0. But RoCE traffic on this BF3 (mlx5_0) reaches the algo on port_num=1
	 * (format-6 events show port=1). Fetching params/counters for
	 * the *event* port (1) therefore reads a never-pushed array (defaults) and strands
	 * counter increments on a port the orchestrator never reads -- which is exactly why
	 * a pushed t_high=500000 read back fine via cmd_type=0xA yet the algo kept using
	 * 80000. Pin to the port the PPCC channel actually targets (0) so a single-port-
	 * per-host deployment sees its pushed params and gets its counters scraped. Mirrors
	 * the "always slot 0" simplification; revisit if a build ever drives multiple ports. */
	const uint32_t TIMELY_PPCC_PORT = 0;
	uint32_t *param = doca_pcc_dev_get_algo_params(TIMELY_PPCC_PORT, 0);
	uint32_t *counter = doca_pcc_dev_get_counters(TIMELY_PPCC_PORT, 0);

	timely_algo(event, param, counter, algo_ctxt, results);

	/* Format 6 (per-event) trace: compiled in, emitted only when the trace_on PPCC
	 * param is set at runtime (or always if built -DTIMELY_DEBUG). See timely.h.
	 * ev_attr is decoded here, under the gate: with params pinned to port 0, the
	 * event's port_num/ev_type are needed only for this trace. */
	if (TIMELY_TRACE_ENABLED(param)) {
		doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
		doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
				     (uint64_t)ev_attr.ev_type,
				     (uint64_t)doca_pcc_dev_get_flowtag(event),
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
 * Unlike DCQCN (which masks ACK events because it relies on CNPs), TIMELY relies on
 * RTT events (ev_type=DOCA_PCC_DEV_EVNT_RTT). We do NOT disable any events; in
 * particular ACK events are kept enabled to ensure no event path is unintentionally
 * blocked. RTT events are produced by the NP in response to results->rtt_req=1.
 *
 * FIXME: if ACK event volume causes performance issues we may want to disable them
 * (TIMELY does not act on ACKs). Verified empirically — measure event rate first
 * before adding masks.
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo.
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	/* Initialize TIMELY algorithm with algo_idx=0 */
	timely_init(algo_idx);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 will use algo_idx 0 (TIMELY), default enabled */
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, 0);
	}

	/* Do not mask any event types; TIMELY needs RTT events to fire. */
	*disable_event_bitmask = 0;
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
			break;
		}
	return ret;
}
