# slot_exp — multi-algo slot-dispatch hardware experiment

**THROWAWAY — do not merge to pre-master.** This app exists to answer one
question on real BlueField-3 hardware: does NVIDIA's documented multi-algorithm
slot machinery (`doca_pcc_dev_init_algo_slot`, `doca_pcc_dev_get_algo_index`,
per-slot param/counter arrays, PPCC `algo_slot` enable/disable) actually work
end-to-end for non-ECE RoCE traffic on this NIC/firmware, before any of that
machinery is relied on by a "real" algorithm branch. Once the experiment has an
answer, this directory (and the `slot-experiment` branch) should be deleted,
not merged into `main` or any algorithm branch (`timely`, `dcqcn`, `swift`, ...).

## What it is

A second, disjoint DOCA PCC RP device app (`--rp-slot-exp`) that registers two
algorithms side by side in the same process:

| algo_idx | algo slot | algo_id  | What it is | Source |
|---|---|---|---|---|
| 0 | 0 | `0xBFFD` (TIMELY's own ID, unchanged) | TIMELY, called **unmodified** | `device/rp/timely/algo/timely.c` (borrowed, not copied — `device/rp/timely/**` is untouched) |
| 1 | 1 | `0xBFFA` | FIXRATE — a fixed-rate stub, not a real congestion controller | `device/rp/slot_exp/algo/fixrate.c` |

`0xBFFA` is the next free algo_id in the project registry below SMaRTT's
`0xBFFB` (see the branch table in the top-level `CLAUDE.md`); it is scoped to
this experiment only and should be freed back up if `slot_exp` is deleted.

`doca_pcc_dev_user_algo` in `rp_slot_exp_dev_main.c` reads
`doca_pcc_dev_get_algo_index(port, attr->algo_slot)` on every event and
switches on it: `0` → `timely_algo()`, `1` → `fixrate_algo()`, anything else →
the stock `rtt_template` internal-algo fallback
(`doca_pcc_dev_default_internal_algo`), covering the NIC's reserved slot 15
for traffic that never negotiated a custom `algo_slot`.

FIXRATE is deliberately trivial: on every event it sets
`results->rate = param[FIXRATE_P_RATE_FXP20]` (default 524288 = 50% of a
FXP20 line rate), never sets `results->rtt_req`, and bumps two counters
(`events` on every call, `flow_inits` once per distinct flow via a
zero-sentinel on its 4-byte per-flow ctxt — the same "ctxt starts zeroed"
convention TIMELY and rtt_template use for their own new-flow detection). It
exists only to be cheaply, unambiguously distinguishable from TIMELY in the
traces and counters — if slot 1's counters and rate track FIXRATE's constant
while slot 0's track TIMELY's RTT-driven behavior, the slot machinery works.

## Trace / counter surface

- Format 0 (shared "user init" trace): emitted twice per port at boot, once
  for each `(algo_idx, algo_slot)` pair registered — `port, idx=0, slot=0`
  then `port, idx=1, slot=1`.
- Format 6 (shared per-event trace) is emitted **ungated, for every event**
  (no `trace_on` param check — this is a short-lived diagnostic binary, not a
  steady-state algorithm): `tsflow=... ev_type=... port=<event's real port>
  algo_slot=<attr->algo_slot> rate=<results->rate>`. `algo_slot` here is the
  whole point: it tells you which slot the infrastructure actually dispatched
  the event to, independent of which port it physically arrived on.
- TIMELY's own formats 11/12/14 still fire from inside `timely_algo()` exactly
  as they do in the standalone `--rp-timely` app, gated by
  `param[TIMELY_P_TRACE_ON]` on slot 0's param array (push it via PPCC same as
  always). FIXRATE has no format of its own; format 6 is all it emits.
- PPCC param/counter access is pinned to **port 0** for both slots (the same
  `mlxreg local_port=1 → DPA port 0` vs. `mlx5_0 traffic → DPA port 1` quirk
  documented in the top-level `CLAUDE.md` applies uniformly regardless of
  which algo occupies the slot).

## Experiment protocol (sketch)

1. Build with both algos wired in (`meson setup --wipe ...`), run
   `doca_pcc --rp-slot-exp -d mlx5_0 -l 70`.
2. Confirm two format-0 lines per port at boot: `idx=0/slot=0` and
   `idx=1/slot=1`, both `algo enable=1`.
3. Drive non-ECE RoCE traffic (`ib_write_bw` or the usual incast-bench sender)
   against the BF3 and watch format-6 traces: every event should show
   `algo_slot=0` or `algo_slot=1` — never anything else, and never crash into
   the `default:` fallback unless traffic that skipped ECE negotiation lands
   on slot 15 as expected.
4. Read back counters for both slots over PPCC (`cmd_type=0xA`/`0xC` style
   reads, `mlxreg-pretty.py` from the `timely` branch decodes them): slot 0's
   `events`/`ai`/`md_*` counters should move the way TIMELY normally does;
   slot 1's `events`/`flow_inits` should move independently, and its `rate`
   counter/trace field should sit at whatever `rate_fxp20` was last pushed,
   never react to RTT.
5. Push a PPCC param write to slot 1 (`rate_fxp20`, param index 0,
   `cmd_type=0xB`) and confirm slot 1's `results->rate` (visible in format 6)
   changes on the next event while slot 0 is unaffected, and vice versa for a
   slot-0 TIMELY param write.
6. Flip `algo_en` (disable one slot, e.g. via a re-push with `algo_en=0` if
   the PPCC command set supports it at runtime, otherwise restart with a
   modified `user_init`) and confirm the disabled slot's events fall through
   to the `default:` fallback rather than continuing to run its old algorithm.
7. Record whichever of the above breaks first — that is the actual answer to
   "does the slot machinery work on this hardware," independent of whether
   FIXRATE or TIMELY's numbers look reasonable.

## Known constraints inherited from the rest of this repo

- Per-flow ctxt capped at 48 bytes (`doca_pcc_dev_algo_ctxt_t`) — FIXRATE's
  ctxt is 4 bytes, nowhere close.
- No float/sqrt/libm on the DPA — FIXRATE does no math at all; TIMELY's own
  fixed-point conventions are unchanged since its source is untouched.
- One ACTIVE PCC process per BlueField (firmware HA) — `slot_exp` cannot run
  alongside any other `--rp-*`/`--np-*` app on the same card.

THROWAWAY — do not merge to pre-master.

## RESULTS (2026-07-14, u50d-01 → u50d-02, non-ECE ib_write_bw)
1. Both slots enabled: 97.40G, fmt-6 = 4551×slot0 (timely ran) — non-ECE traffic tags slot 0.
2. Runtime PPCC 0x2 disable slot0 (readback 0x3 confirmed 1→0): traffic STILL slot0,
   timely still ran (95.01G). Enable/disable does NOT retarget non-ECE flows.
3. Slot0 disabled at BOOT (this commit): fixrate (slot1) NEVER ran; traffic split
   2726×slot0 (disabled timely at rate floor 64) + 619×slot15 with rate>2^20 =
   firmware INTERNAL DEFAULT took the flow. 81.24G. Boot enables don't select either.
4. PPCC 0xB param write addressed to algo_slot=1 was ACCEPTED — slots work as
   param-table namespaces even though traffic can't route to them.
CONCLUSION: on BF3 + non-ECE RoCE, user-slot selection (runtime or boot) is not
available; slot 0 is the de-facto default, disabled slot 0 → internal default (slot 15).
Multi-algo design therefore: slots 0-3 = native per-algo param/counter namespaces,
slot 4 = control table (active_algo), dispatch in user_algo on the control param.
