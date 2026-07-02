// DPA entry point

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "dcqcn.h"

#define DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_ACK)
#define PCC_TRACE_FMT_EVENT (6)

/*
 * Main entry point of CC algorithm
 * This function starts the algorithm code of a single event.
 * It receives the flow context data, the event info and outputs the new rate parameters.
 * The function dispatches to DCQCN for slot 0 and to the default internal algo for other slots.
 *
 * @algo_ctxt [in]: A pointer to a flow context data retrieved by libpcc.
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @attr [in]: A pointer to additional parameters (algo type).
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			    doca_pcc_dev_event_t *event,
			    const doca_pcc_dev_attr_t *attr,
			    doca_pcc_dev_results_t *results) {

	/* Always slot 0 (DCQCN is the only algo, so ECE slot negotiation is moot) and
	 * always port 0. The orchestrator pushes params / scrapes counters over mlxreg
	 * PPCC local_port=1, which maps to DPA port 0, but mlx5_0 traffic reaches the algo
	 * on port 1 -- fetching the event's port would read a never-pushed array and
	 * strand counters where the scrape can't see them (HW-confirmed on timely). */
	uint32_t *param = doca_pcc_dev_get_algo_params(0, 0);
	uint32_t *counter = doca_pcc_dev_get_counters(0, 0);

	dcqcn_algo(event, param, counter, algo_ctxt, results);

	/* Per-event trace is hot-path (every TX); gate on DCQCN_DEBUG like the dcqcn.c
	 * traces so a default build pays nothing. */
#ifdef DCQCN_DEBUG
	doca_pcc_dev_trace_5(PCC_TRACE_FMT_EVENT,
			     (uint64_t)doca_pcc_dev_get_ev_attr(event).ev_type,
			     (uint64_t)doca_pcc_dev_get_flowtag(event),
			     (uint64_t)doca_pcc_dev_get_ev_attr(event).port_num,
			     (uint64_t)attr->algo_slot,
			     (uint64_t)results->rate);

	doca_pcc_dev_trace_flush();
#endif
}

/*
 * Main entry point to user algorithm initialization
 * This function starts the user algorithm initialization code.
 * The function will be called once per process load and should init all supported
 * algorithms and all ports.
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo.
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask) {

	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	/* Initialize DCQCN algorithm with algo_idx=0 */
	dcqcn_init(algo_idx);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 will use algo_idx 0 (DCQCN), default enabled */
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK);
	}

	/* Disable ACK events — DCQCN does not use per-ACK feedback */
	*disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK;
	doca_pcc_dev_printf("[DCQCN] %s: init complete, disable_event_bitmask=0x%x\n", __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
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
						       uint32_t *params) {

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
		default:
			/* A set targeting a non-zero slot is a misroute; fail loudly. */
			ret = DOCA_PCC_DEV_STATUS_FAIL;
			break;
		}
	return ret;
}
