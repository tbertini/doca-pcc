# Swift-CX8 — paper-faithful Swift, actuated as a real credit window

Implementation of the RP (Reaction Point) algorithm from
**Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
Datacenter", SIGCOMM 2020** (DOI [10.1145/3387514.3406591](https://doi.org/10.1145/3387514.3406591)) —
**Algorithm 1** (§3.2) with the flow-scaled **target delay** of §3.5.

This is the *second* Swift port in this tree. [`device/rp/swift/`](../swift/README.md) is
the one that runs on BlueField-3: it keeps `cwnd` as the control variable and maps it
onto the rate limiter via the BDP identity. **Swift-CX8 removes that mapping.** It
actuates the window as a window (`results->credits`), opens the rate limiter, and moves
the AIMD controller into the ACK handler where the paper puts it — everything the DOCA
PCC model would allow *if* the credit fields reached the hardware.

---

## ⚠️ READ THIS BEFORE RUNNING IT

**`doca_pcc_dev_results_t.credits` and `.reload_credits` are not implemented on any
DOCA 2.10 chip.** Disassembly of `libdoca_pcc_dev.a` shows the post-algorithm glue
loading only `results->rate` (offset 0) and `results->rtt_req` (offset 4), plus
`probe_type_slot` (offset 8) on ConnectX-8. Offsets 12 and 16 — `credits` and
`reload_credits` — are never read.

Third-party confirmation: Huang et al., **"SwCC"**, USENIX ATC 2025, §3.4 — *"PCC only
allows developers to adjust per-flow rates and cannot support credit-based or
window-based CCAs."*

**Consequence on BlueField-3:** every credit write below is a silent no-op, and because
this variant deliberately pins `rate = MAX_RATE` whenever `cwnd ≥ 1` packet (letting the
window govern instead of the limiter), it **degenerates to an uncontrolled sender**. It
will compile, load, run, count events, and emit traces — with no congestion control.

> **Swift-CX8 is a reference implementation and a thesis artefact. It is NOT a benchmark
> target on current hardware. For measurements on this card use
> [`device/rp/swift/`](../swift/README.md).**

The name is aspirational: it marks the variant as the one to try first on silicon that
honours a credit actuator (ConnectX-8-class or later). Nothing in it is CX-8-specific
today, and NVIDIA has published nothing confirming CX-8 implements the fields.

---

## What is more paper-faithful here

Two changes versus `device/rp/swift/`; everything else — target-delay math, flow scaling,
probe machine, NACK handling, params contract — is identical.

| | `swift` | `swift_cx8` |
|---|---|---|
| **Actuation** | `cwnd` → rate via BDP identity, paced by the measured fabric RTT | `cwnd` → `results->credits`; rate limiter opened to `MAX_RATE`, transmission ACK-clocked (the paper's model) |
| **Where AIMD runs** | once per **RTT-probe** event (one `+ai` step per probe round-trip) | on every **ACK** event, with the paper's `(ai/cwnd)·num_acked` increase (Algorithm 1 lines 7–11) |
| **RTT event does** | measure *and* control | measure only — refresh `last_fabric_rtt`, re-arm the probe |

The per-ACK placement is the substantive one. ACKs coalesce ~5 packets at line rate on
this fabric, so the window now moves at ACK granularity and one full window of ACKs adds
exactly `ai` — which is what Algorithm 1's `ai/cwnd` scaling means and what a
once-per-probe `+ai` cannot reproduce.

### What is still deviant (inherited, unavoidable)

The delay signal comes from explicit **RTT probes** (responder-reflected `t2`/`t4`, sender
decomposes), not from per-ACK timestamps: BlueField-3 exposes no per-ACK delay. So the
ACK handler regulates on the most recent probe sample, refreshed once per probe
round-trip. v1 is also **fabric-only** — the endpoint window (`ecwnd`, §3.3) is reserved
in the context but unused.

---

## Two undocumented assumptions (both `FIXME` in `swift_cx8.c`)

Neither is resolvable from any public source: the fields appear only as one-liners in
`doca_pcc_dev.h:75-76`, have no getter/setter helpers, appear in no NVIDIA sample or
public code, and have no backing field in the BF3 little-endian result struct to inspect.
Checked across DOCA 2.9.0-cx8 and 3.4.0 documentation, GitHub, and the NVIDIA forums
(2026-08-15).

1. **Credit unit.** `credits` is assumed to count **whole packets**, matching `cwnd`'s
   unit. It could be bytes or MTUs. If it turns out to be bytes, multiply by
   `param[SWIFT_CX8_P_MSS_BYTES]` inside `swift_cx8_actuate()` and nowhere else.
2. **`reload_credits` semantics.** Set equal to `credits`, i.e. read as *"the value the
   credit bucket refills to"*. The rival reading is an edge-triggered *"reload now"*
   command, which would be `0`/`1` rather than a quantity.

---

## File layout

| File | Purpose |
|---|---|
| `algo/swift_cx8.h` | Public API: `swift_cx8_init`, `swift_cx8_algo`, `swift_cx8_set_algo_params`; trace gate, format IDs, branch bits |
| `algo/swift_cx8_ctxt.h` | Per-flow state, **exactly 48 bytes** — adds `last_ack_psn` + `flags` (the `num_acked` estimator) over swift's layout, spending two of its three reserved words |
| `algo/swift_cx8_algo_params.h` | Compiled-in defaults; every value cites a paper section or is marked `FIXME:` |
| `algo/swift_cx8_params.h` | The PPCC **contract**: 20-param (19 tuning + `trace_on`) / 13-counter table. Byte-identical to swift's except `SWIFT_CX8_ALGO_ID = 0xBFFA` |
| `algo/swift_cx8.c` | Algorithm 1 + §3.5 target; `swift_cx8_actuate()` is the credit actuator every handler ends on |
| `rp_swift_cx8_dev_main.c` | DPA entry points; port-0 pinning for params/counters |

## Build & run

Device code is compiled by `dpacc` during `meson setup`, **not** during `ninja` — after
any change under `algo/`, to `rp_swift_cx8_dev_main.c`, or to `build_device_code.sh`,
re-run `meson setup --wipe` or a bare `ninja` will silently reuse the stale device binary.

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc --rp-swift-cx8 -d mlx5_0 -l 70    # short form: -rp-sw8
```

Traces (formats 6, 15–17, 19) are compiled in but **quiet by default**; format 18
(loss/NACK) is always emitted. Toggle the rest at runtime via the `trace_on` PPCC param —
no rebuild. To force them on from boot:

```bash
SWIFT_CX8_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

`--rp-swift-cx8` and `--rp-swift` select different device binaries from the same host
executable; one ACTIVE PCC process per BlueField, so they cannot run concurrently.

---

## The credit actuator

`swift_cx8_actuate()` is called as the **last step of every handler**, so every exit path
leaves `results` fully written:

```c
if (cwnd >= 1 packet) {
        credits = round(cwnd);              /* FXP16 -> whole packets */
        rate    = DOCA_PCC_DEV_MAX_RATE;    /* window governs, not the limiter */
} else {
        credits = 1;                        /* a fraction of a packet is inexpressible */
        rate    = cwnd_to_rate(cwnd, last_fabric_rtt);   /* pacing_delay = rtt/cwnd */
}
results->rate = rate;  results->credits = credits;  results->reload_credits = credits;
```

Above one packet the rate limiter is out of the loop entirely — this is the ACK-clocked
model the paper assumes, and the reason the variant is uncontrolled when credits are
ignored. Below one packet a credit window cannot express the state, so the FXP20 rate
takes over and implements Algorithm 1 lines 29–32 (`pacing_delay = rtt/cwnd`), dividing
by the last measured fabric RTT exactly as `device/rp/swift/` does.

`rtt_req` is deliberately **not** set by the actuator: the probe state machine owns it and
each handler sets it for itself.

Consequences for the shared diagnostics, both visible in traces and counters:
- `rate_clamped_max` now moves on essentially every event above one packet (the rate *is*
  `MAX_RATE`), and `rate_clamped_min` only in the sub-packet pacing regime.
- The `CLAMPED` trace bit reports **the window** hitting `min_cwnd`/`max_cwnd`, not the
  rate — those bounds are the only guards left.

---

## Event → Algorithm 1 mapping

| Event | Handler does | Algorithm 1 |
|---|---|---|
| **new flow** (`cur_rate == 0` sentinel) | `fcwnd = init_cwnd`, zero the probe + PSN state, actuate, `rtt_req = 1` | §3.2 (initial cwnd has little effect) |
| **ACK** — *the main handler* | `retx_cnt ← 0`; `num_acked` from wrap-safe ΔPSN; **AI or MD on `last_fabric_rtt` vs the flow-scaled target**; clamp; `t_last_decrease` update; actuate; `rtt_req = 0` | lines 4–28 (+ §3.5 target) |
| **RTT** (probe response) | extract `t1/t2/t4/t6`, `fabric_rtt = (t6−t1) − (t4−t2)`, store as `last_fabric_rtt`, `retx_cnt ← 0`, actuate unchanged window, `rtt_req = 1` | §3.1/§3.3 measurement only — **no control** |
| **TX** | probe abort/retry state machine (300 µs abort, backoff); no window change | — (DOCA model mapping) |
| **NACK** | `retx_cnt++`; at `retx_reset_thresh` → `cwnd = min_cwnd`, else once-per-RTT `cwnd ·= (1 − max_mdf)` | §3.6 / "On Retransmit Timeout" |
| **CNP** | ignored (Swift is delay-based; §3.7 isolates via QoS) — re-actuate unchanged | — |
| default | re-actuate unchanged, `rtt_req = 0` | — |

### Inside the ACK handler

```
num_acked = wrap_safe(psn − last_ack_psn), capped at max(2·cwnd_pkts + 2, 256)
            fallback to ev_attr.num_coalesced on the first ACK or an absurd delta
delay     = last_fabric_rtt
if delay == 0:  actuate, return          # no probe sample yet — do NOT regulate
target    = base_target + fs_term(cwnd) + num_hops·hop_scale
can_decrease = (now − t_last_decrease) >= delay
if delay < target:                        # AI, lines 7–11
    cwnd += (cwnd >= 1) ? (ai/cwnd)·num_acked : ai·num_acked
elif can_decrease:                        # MD, lines 12–14
    cwnd ·= max(1 − β·(delay−target)/delay, 1 − max_mdf)
else: MD_BLOCKED                          # no change
cwnd = clamp(min_cwnd, cwnd, max_cwnd)    # line 26
if cwnd <= cwnd_prev: t_last_decrease = now   # lines 27–28
actuate(cwnd)                             # lines 29–32 + output
```

Three things worth knowing about this handler:

- **`delay == 0` parks the controller.** No usable probe sample has landed (or every
  sample so far was dropped for missing responder stamps), so there is nothing to compare
  against the target. Holding the window is the honest response; treating "unknown" as
  "uncongested" would ramp blindly. A non-stamping responder therefore leaves Swift-CX8
  visibly **inert** — `ai`/`md` frozen, `rtt_dropped_unstamped` climbing.
- **The `can_decrease` guard is now load-bearing.** Many ACKs arrive within one RTT;
  without the guard every one of them would cut the window and a single congestion signal
  would become a collapse. In the probe-driven variant the guard was ~always true. Its
  period is the fabric RTT — slightly shorter than the true round trip, making the guard
  marginally permissive rather than marginally strict.
- **`num_acked` is an estimate.** The ΔPSN idiom is ported from
  `smartt_reps_psn_delta()`; a reordered or stale PSN falls back to the event's own
  `num_coalesced`, and the result is capped at ~two windows so a bad sample cannot inject
  an enormous increase. `num_acked == 0` adds nothing, which is correct.

---

## Trace format reference

Format IDs **15–19 are shared with `device/rp/swift/`, deliberately.** Only one PCC app
runs per card, the host-side `trace_message_formats[]` table (`host/pcc_core.c`) is a
single shared array, and the two Swift variants are never active simultaneously — so
`swift_cx8` reuses swift's strings verbatim and the host table is **untouched** by this
variant. Format 6 (per-event summary) is the shared generic one.

The field *lists* are identical to swift's; **who emits them and when is not**:

| ID | Emitted by | Fields (`arg1…arg5`) |
|---|---|---|
| 6 | every event (`rp_swift_cx8_dev_main.c`) | `tsflow`; `ev_type`; `port`; `algo_slot`; `rate` (FXP20 after algo) |
| 15 | `new_flow` | `tsflow`; `init_cwnd` (FXP16 pkts); `line_mbps`; `bdp_fxp16`; `rate` |
| 16 | **ACK handler** (delay side) | `tsflow`; `fabric_rtt` (ns); **`num_acked`** ⚠️; `target` (ns); `branch` |
| 17 | **ACK handler** (window side) | `tsflow`; `cwnd` (FXP16 pkts); `fs_term` (ns, = target − base); `can_decr`; `rate` |
| 18 | NACK — **always emitted**, not gated by `trace_on` | `tsflow`; `ev_subtype` (1=RNR, 2=OOS, 3=DUP_READ); `retx_reset` (bits 31:1 = `retx_cnt`, bit0 = reset-to-min); `cwnd`; `rate` |
| 19 | **RTT handler**, now always standalone | `tsflow` (ts half = `t6`/arrival); `t1_req_send`; `t2_req_recv`; `t4_resp_send`; `populated` (bit0 = t2≠0, bit1 = t4≠0; **3 = usable**, else dropped) |

`tsflow` packs `ts_ns` (bits 63:32) and `flow_tag` (bits 31:0); lines from one event carry
an identical `tsflow`, so a reader joins them by value rather than by log order.

**⚠️ Field remap on format 16.** Its third slot carries **`num_acked`** here, not
`np_turnaround` — an ACK event decomposes no probe. The shared host format string still
prints it under the label **`np_turn=`**. A `swift_cx8` capture read with a swift-era
prettifier will show a small integer (typically 1–8) where nanoseconds are expected;
that is `num_acked`, and it is the tell that you are looking at a CX8 trace.

**Sampling.** Formats 16/17 fire on **every** `MD` and `MD_BLOCKED` decision, but on
additive increase only when `(psn & 63) == 0` — roughly 1 in 64. An AI line per ACK would
melt the tracer at line rate. Decreases are rare and individually interesting, so they are
never sampled out. Format 19 fires on every probe response, including dropped ones (it is
the diagnostic that explains the drops).

### `branch` (format 16; low nibble = Algorithm-1 path, high bits OR-ed in)

| branch | Condition | Action |
|---|---|---|
| `0x00` `AI` | `delay < target` | `cwnd += (ai/cwnd)·num_acked` (or `ai·num_acked` below 1 pkt) |
| `0x01` `MD` | `delay ≥ target`, `can_decrease` | `cwnd ← max(1−β·(delay−target)/delay, 1−max_mdf)·cwnd` |
| `0x02` `MD_BLOCKED` | `delay ≥ target`, guard blocked it | no change |
| `0x80` bit | — | `cwnd` clamped to `min_cwnd`/`max_cwnd` (**the window**, not the rate — the limiter is pinned open above 1 pkt) |
| `0x40` bit | — | `cwnd` ended `< 1` pkt (pacing regime) |
| `0x20` bit | — | flow-scale term saturated (0 or `fs_range`) |

### `ev_type` (format 6)

| ev_type | Name | Role here |
|---|---|---|
| 2 | `CNP` | ignored |
| 3 | `TX` | probe abort machine |
| 4 | `ACK` | **the control loop** |
| 5 | `NACK` | loss signal (format 18) |
| 6 | `RTT` | delay measurement only (format 19) |

Note the inversion versus `device/rp/swift/`, where `RTT` was the main signal and `ACK`
only reset `retx_cnt`.

---

## Params and counters

Identical contract to `device/rp/swift/` — same 20 params in the same order, same 13
counters, same defaults, same units. Only `SWIFT_CX8_ALGO_ID` differs (`0xBFFA` vs
`0xBFFC`). No new params in v1; the table is append-only and `trace_on` stays last.

That means the same tuning advice applies, in particular: **retune `base_target` from a
no-load `fabric_rtt` trace.** Idle fabric RTT on this cluster measures ~3–4 µs, not the
paper's 25 µs, so the paper's target would never trigger a decrease. Default seed is
10 µs. See [swift's tuning section](../swift/README.md#tuning-parameters-for-your-fabric)
for the full discussion — none of it changes here.

Two defaults are worth re-examining *specifically* on a card where credits work, because
per-ACK AIMD changes their meaning:

- **`ai`** is now spread across a window of ACKs rather than applied once per probe. The
  effective per-RTT increase is `ai` by construction (that is the point of the `ai/cwnd`
  scaling), but the *cadence* is much finer, so a value tuned against the probe-driven
  variant is not a like-for-like seed.
- **`min_rtt_ns`** feeds the NACK handler's once-per-RTT guard and the BDP; the ACK
  handler's guard uses the live `last_fabric_rtt` instead.

Host-side contract dump for orchestrators (JSON, no DOCA deps):

```bash
/tmp/build/pcc/swift_cx8_params_dump
```

Runtime override goes through PPCC exactly as for swift — push with
`mlxreg ... local_port=1 ... cmd_type=0xB`, scrape counters with `0xC`/`0xD`.
`local_port=1` maps to DPA port 0, which is where this algorithm pins its params and
counters; pushing anywhere else silently no-ops.

---

## Known unknowns / FIXMEs

`grep -rn FIXME device/rp/swift_cx8/` is the live list. The ones specific to this variant:

- **Credit unit** and **`reload_credits` semantics** (`swift_cx8.c`, `swift_cx8_actuate`)
  — see above. Both are unresolvable without silicon that honours the fields.
- **`num_acked` fidelity.** ΔPSN counts packets acknowledged, which is what Algorithm 1
  wants, but a coalesced ACK covering a retransmitted range can over-count. The cap
  bounds the damage; it does not eliminate it.
- **NACK subtype split** (inherited). `ev_subtype` *is* populated (measured 2026-07-06:
  ramp NACKs arrive as OOS=2), so the paper's fast-recovery/RTO split is implementable;
  currently every NACK takes the conservative RTO path.
- **Endpoint window** `ecwnd` (§3.3) — reserved in the context, unimplemented.
- **`__dpa_fxp_log2`/`__dpa_fxp_pow2` precision** for sub-1 inputs in the `1/√cwnd`
  flow-scale term (inherited from swift).

## Hardware / branch context

- algo_id **`0xBFFA`**. Registry: `0xBFFF` rtt_template, `0xBFFE` dcqcn, `0xBFFD` timely,
  `0xBFFC` swift, `0xBFFB` smartt_reps.
- Trace IDs **15–19 shared with swift by design** (see above) — the only shared-ID pair in
  the registry. Everything else in this tree is a disjoint sibling.
- Build/host glue is additive: `build_device_code.sh` (own `elif` block, `SWIFT_CX8_DEBUG`
  flag), `meson.build` (device app + `swift_cx8_params_dump`), `pcc_core.c`
  (`--rp-swift-cx8` / `-rp-sw8`), `pcc_core.h` (extern). The default app in `host/pcc.c`
  is unchanged.
- **Compile-checked only.** This variant has never regulated traffic, because no available
  card implements its actuator.
