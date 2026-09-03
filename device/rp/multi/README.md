# Multi-algorithm PCC app (`--rp-multi`)

One DPA binary that carries all four congestion-control algorithms (DCQCN, TIMELY,
Swift, SMaRTT) and switches between them at runtime through a PPCC register write,
with no process restart. This document is the frozen specification; the
implementation must not deviate from the registry below without updating this file.

## Motivation

The single-algorithm apps (`--rp-dcqcn`, `--rp-timely`, `--rp-swift`,
`--rp-smartt-reps`) each load one DPA ELF; changing algorithm means killing and
restarting `doca_pcc`. Parameters already flip live via PPCC `cmd_type=0xB`. This app
extends the same mechanism to algorithm selection: a fifth, minimal "mux" algorithm
occupies its own slot and holds a single `algo_select` parameter; the per-event
handler reads it and dispatches to the selected algorithm.

## Frozen registry

| Item | Value |
|---|---|
| App name (dpacc/meson) | `pcc_rp_multi_app` |
| CLI flag | `--rp-multi` / short `-rp-mt` |
| Branch | `multi-algo` (from `pre-master`) |
| Mux algo_id | `0xBFFA` (next free below 0xBFFB) |
| Mux trace format ID | 25 (next free after SMaRTT's 20-24) |
| Slot/idx 0 | DCQCN (algo_id 0xBFFE) |
| Slot/idx 1 | TIMELY (algo_id 0xBFFD) |
| Slot/idx 2 | Swift (algo_id 0xBFFC) |
| Slot/idx 3 | SMaRTT (algo_id 0xBFFB) |
| Slot/idx 4 | mux (algo_id 0xBFFA) |

`algo_idx == algo_slot` for all five, on every port. The mux contract lives in
`multi_mux_params.h`: param index 0 = `algo_select`, uint32, default 0, min 0, max 3.
The mux has 1 placeholder counter (index 0, `reserved0`, never written). The param
table is append-only like every other PPCC contract.

## Device side (`rp_multi_dev_main.c`)

The four algorithm cores are compiled in unchanged from
`device/rp/{dcqcn,timely,swift,smartt_reps}/algo/*.c`; only their per-app
`rp_*_dev_main.c` files are excluded (they define the same `doca_pcc_dev_user_*`
entry points this file defines). All exported algo symbols are prefixed
(`dcqcn_*`, `timely_*`, `swift_*`, `smartt_reps_*`); verified no collisions.

### `doca_pcc_dev_user_init`

1. Register all five algorithms once:
   `dcqcn_init(0); timely_init(1); swift_init(2); smartt_reps_init(3); multi_mux_init(4);`
   `multi_mux_init` mirrors `dcqcn_init` (dcqcn.c:114) with algo_id 0xBFFA,
   1 param, 1 counter, desc "multi-algo mux v1.0".
2. For every port and every slot 0..4:
   `doca_pcc_dev_init_algo_slot(port, slot, slot, algo_en);` with a local
   `uint32_t algo_en = 1;` (the same pattern all four single apps use; no named
   enable constant exists in the DOCA headers), then the same format-0 init trace
   the single apps emit.
3. `*disable_event_bitmask = 0` - the union of the four algorithms' needs.
   SMaRTT consumes ACK events and masks nothing; therefore nothing can be masked.
   Every algorithm receives event types it would normally mask; all four handlers
   end in a catch-all `else` that passes through `cur_rate` with `rtt_req = 0`,
   so foreign events are cheap no-ops (verified on all four cores).

### `doca_pcc_dev_user_algo`

1. `mux_param = doca_pcc_dev_get_algo_params(0, 4);` - port pinned to 0 (the
   established PPCC quirk: `mlxreg local_port=1` writes DPA port 0, so all
   params/counters are pinned to port 0 exactly like the single apps).
2. `sel = mux_param[0]; if (sel > 3) sel = 0;` (defense in depth; the setter
   already validates).
3. `param = doca_pcc_dev_get_algo_params(0, sel); counter = doca_pcc_dev_get_counters(0, sel);`
4. `switch (sel)`: call `dcqcn_algo` / `timely_algo` / `swift_algo` /
   `smartt_reps_algo` with `(event, param, counter, algo_ctxt, results)`.
5. Per-event format-6 trace: each case replicates its single app's trace block
   verbatim (its own `*_TRACE_ENABLED(param)` gate and `*_TRACE_TSFLOW` packing,
   and any `#ifdef *_DEBUG` wrapping present in the original main).

The negotiated `attr->algo_slot` is deliberately ignored: benchmark traffic is
non-ECE and lands on slot 15 anyway; selection is global via the mux.

### `doca_pcc_dev_user_set_algo_params`

`switch (algo_slot)`:
- Cases 0..3: mirror the single apps - fetch
  `doca_pcc_dev_get_algo_index(port_num, algo_slot)`, check it equals the slot,
  call that algorithm's `*_set_algo_params(param_id_base, param_num, new_param_values, params)`.
- Case 4: bounds-check `param_id_base`/`param_num` against the 1-entry table and
  validate every written `algo_select` value is <= 3 (return
  `DOCA_PCC_DEV_STATUS_FAIL` otherwise), emit trace format 25 with old and new
  value, then copy the values into `params` and return OK. The framework does NOT
  auto-copy: the setter callback must write the live array itself (contract
  documented identically in all four algorithms' setters, e.g. dcqcn.c:525).
- Default: `DOCA_PCC_DEV_STATUS_FAIL`.

Trace format 25 (appended to the host table in `pcc_core.c`, append-only):

```
"format 25 - multi mux: old_select=%lu new_select=%lu port=%lu slot=%lu. end %u\n"
```

### Live-switch semantics (accepted design)

Per-flow context (48 bytes) is NOT translated on switch. All four context structs
begin with `uint32_t cur_rate` (FXP20) at offset 0, and all four treat
`cur_rate == 0` as the new-flow sentinel. So after a switch the new algorithm
inherits a valid live rate and misreads the remaining 44 bytes as its own state
for the events until it has overwritten them ("one-event glitch"). This is
accepted and documented; flows converge within a few events.

Parameters are per-slot, so the next algorithm's parameters can be staged via 0xB
before flipping the selector.

## Host side

- `pcc_core.h`: `extern struct doca_pcc_app *pcc_rp_multi_app;`
- `pcc_core.c`: `rp_multi_callback` + registration (mirror the DCQCN flag block;
  boolean, long name `rp-multi`, short `rp-mt`); append format 25 to
  `trace_message_formats` before the NULL terminator.
- `meson.build`: one more dpacc stanza (mirror the Swift one) + add
  `pcc_rp_multi_app` to the executable's dependency list.
- `build_device_code.sh`: new `elif` block for `pcc_rp_multi_app`:
  sources = `device/rp/multi/*.c` plus the four `device/rp/<algo>/algo/*.c` globs
  (NOT the four `rp_*_dev_main.c`); include paths for all four algo dirs plus
  `device/rp/multi`; honor all four `*_DEBUG` env flags exactly as the single
  blocks do (`DCQCN_DEBUG`, `TIMELY_DEBUG`, `SWIFT_DEBUG`, `SMARTT_REPS_DEBUG`).
  `PCC_TRACE_FMT_EVENT (6)` is defined only in the excluded single mains, so
  `rp_multi_dev_main.c` defines its own copy plus `PCC_TRACE_FMT_MUX (25)`.

No `multi_params_dump` binary in v1; verification drives the mux with raw
`mlxreg` writes. incast-bench integration is a follow-up.

## Runtime operation

```bash
/tmp/build/pcc/doca_pcc --rp-multi -d mlx5_0 -l 70
# stage parameters for slot S (0..3):  mlxreg ... PPCC cmd_type=0xB, algo_slot=S, ...
# switch algorithm:                    mlxreg ... PPCC cmd_type=0xB, algo_slot=4, value[0]=S
# read/clear counters of slot S:       mlxreg ... PPCC cmd_type=0xD, algo_slot=S
```

Only the selected slot's counters move under traffic; that is the primary
verification signal (aggregate bandwidth is receiver-bound at 2:1 and hides
algorithm differences).

## Watchlist (verify empirically on hardware)

- Five `doca_pcc_dev_algo_init_metadata` registrations coexist (DOCA supports 8
  algos / 15 slots; never exercised five at once here before).
- A 0xB write to `algo_slot=4` reaches the device setter and format 25 appears
  in `-l 70` output.
- After switching to a windowed algorithm (Swift/TIMELY/SMaRTT) from DCQCN,
  RTT-probe requests resume (`rtt_req` cadence) on live flows.
- Counter movement follows the selector; craterable via each algorithm's own
  params (propagation proof per algorithm).
- One-event glitch does not persist (rate recovers within a few events after a
  mid-traffic flip).
