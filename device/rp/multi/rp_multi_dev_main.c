// DPA entry point

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>
#include "pcc_common_dev.h"
#include "dcqcn.h"
#include "timely.h"
#include "swift.h"
#include "smartt_reps.h"
#include "multi_mux_params.h"

/* Shared per-event summary trace (see pcc_core.c trace_message_formats[]). Defined
 * here, not pulled from one of the four single apps: this file never includes any
 * rp_<algo>_dev_main.c (they all define the doca_pcc_dev_user_* entry points this
 * file defines), and none of the four algo headers define it either. */
#define PCC_TRACE_FMT_EVENT (6)

/* Mux param-change trace (device/rp/multi/README.md): old_select, new_select,
 * port, slot. */
#define PCC_TRACE_FMT_MUX (25)

static const volatile char multi_mux_desc[] = "multi-algo mux v1.0";
static const volatile char multi_mux_param_algo_select_desc[] =
	"ALGO_SELECT, active algorithm slot: 0=DCQCN 1=TIMELY 2=Swift 3=SMaRTT";
static const volatile char multi_mux_counter_reserved0_desc[] = "RESERVED0, unused placeholder counter";

/*
 * Registers the mux as its own algo (algo_idx 4): no congestion-control logic of
 * its own, just the algo_select param that doca_pcc_dev_user_algo() reads every
 * event to pick which of the four real algorithms (registered at algo_idx 0-3
 * below) handles it. Mirrors dcqcn_init() (dcqcn.c), inlined here since the mux has
 * no algo/ directory of its own; see multi_mux_params.h for the contract.
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
 * Reads the mux's algo_select param (slot 4, port 0) and dispatches the event to
 * whichever of the four real algorithms is currently selected, using that
 * algorithm's own slot params/counters (slot == algo_idx == the selector value,
 * pinned to port 0: the same PPCC port mapping quirk every single-algo app in this
 * repo works around, since mlxreg local_port=1 writes DPA port 0 while mlx5_0
 * traffic arrives on DPA port 1). The negotiated attr->algo_slot is deliberately
 * ignored: benchmark traffic is non-ECE and lands on slot 15 regardless, so
 * selection is global via the mux (device/rp/multi/README.md).
 *
 * Each case replicates its single app's format-6 trace block verbatim (its own
 * *_TRACE_ENABLED(param) gate and *_TRACE_TSFLOW packing).
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

	/* Defense in depth: doca_pcc_dev_user_set_algo_params() (case 4 below) already
	 * rejects any algo_select write above 3, so this should never trigger. */
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
		/* Unreachable: sel is clamped to [0,3] above. Fall back to DCQCN rather
		 * than leaving *results unset if this is ever reached. */
		dcqcn_algo(event, param, counter, algo_ctxt, results);
		break;
	}
}

/*
 * Main entry point to user algorithm initialization.
 * Called once per process load; registers all four algorithms plus the mux, and
 * enables all five slots on every port.
 *
 * The four algorithms keep the algo_idx they use in their single-algo apps
 * (DCQCN=0, TIMELY=1, Swift=2, SMaRTT=3); the mux is a fifth, minimal algorithm at
 * slot 4 whose only job is to be read by doca_pcc_dev_user_algo() to pick which of
 * the other four handles the current event (device/rp/multi/README.md).
 *
 * *disable_event_bitmask is the union of what the four algorithms need: SMaRTT is
 * this repo's first ACK-event consumer and masks nothing, so nothing can be masked
 * here either, or SMaRTT's control loop (which runs on ACK events) would starve
 * whenever it is selected. All four algorithms' handlers already end in a catch-all
 * else branch that passes cur_rate through with rtt_req=0 for event types they
 * don't otherwise use, so receiving unmasked events is a cheap no-op for the other
 * three (verified on all four cores).
 *
 * @disable_event_bitmask [out]: event types to mask out (dropped before any algo).
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_en = 1;

	/* Register all five algorithms once. */
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

	/* Union of the four algorithms' needs is "mask nothing" (SMaRTT). */
	*disable_event_bitmask = 0;
	doca_pcc_dev_printf("[MULTI] %s: init complete, disable_event_bitmask=0x%x\n",
			    __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
}

/*
 * Called when a parameter change was set externally. Validates the new values and,
 * if legal, the framework assigns them to the parameter array.
 *
 * Cases 0-3 mirror the single apps unchanged: each fetches the live algo_idx for
 * that slot, confirms it still matches the slot number, and dispatches to that
 * algorithm's own *_set_algo_params(). Case 4 is the mux itself: bounds-checks
 * param_id_base/param_num against its 1-entry contract, rejects any algo_select
 * value above 3, emits trace format 25 with the old and new value, then writes the
 * validated values into params itself (the framework does not auto-copy: contract
 * documented identically in all four algorithms' setters, e.g. dcqcn.c).
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
		/* A PPCC set targeting a slot other than 0-4 is a misroute; fail loudly. */
		ret = DOCA_PCC_DEV_STATUS_FAIL;
		break;
	}
	return ret;
}
