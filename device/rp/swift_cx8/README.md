# Swift-CX8 — paper-faithful credit-window Swift (reference variant)

algo_id `0xBFFA`. A sibling of [`device/rp/swift/`](../swift/README.md) that stays as
close to the Swift paper (Kumar et al., SIGCOMM 2020) as the DOCA PCC model allows
**under the assumption that `doca_pcc_dev_results_t.credits` / `.reload_credits`
actually reach the hardware**.

## Why it exists — and why it is not a benchmark target

On every DOCA 2.10 chip the credit fields do **not** reach the hardware: disassembly of
`libdoca_pcc_dev.a` shows the post-algorithm glue loading only `results->rate`
(offset 0) and `results->rtt_req` (offset 4), plus `probe_type_slot` (offset 8) on
ConnectX-8; offsets 12 and 16 (`credits`, `reload_credits`) are never read. Third-party
confirmation: Huang et al., "SwCC", USENIX ATC 2025, §3.4 — PCC only allows adjusting
per-flow rates and cannot support credit- or window-based CCAs.

Consequence: because this variant deliberately opens the rate limiter
(`rate = MAX_RATE`) whenever `cwnd >= 1` packet — letting the window, not the limiter,
govern — the silent no-op credit writes leave it an **uncontrolled sender on current
silicon**. It is a reference implementation and a thesis artefact. For measurements on
this hardware use `device/rp/swift/`, which maps the same cwnd onto the rate limiter
via the BDP identity. See the "READ THIS FIRST" block in
[`algo/swift_cx8.c`](algo/swift_cx8.c).

## Differences vs `device/rp/swift/`

- **Window actuated AS a window:** `results->credits` carries cwnd and transmission is
  ACK-clocked, which is what the paper assumes. Below one packet a credit count cannot
  express the fractional window, so the FXP20 rate takes over and implements the
  paper's `pacing_delay = rtt/cwnd` regime (Algorithm 1 lines 29–32).
- **Per-ACK AIMD:** the control loop runs in the ACK handler with the paper's
  `(ai/cwnd)·num_acked` increase (Algorithm 1 "On Receiving ACK"), instead of one `+ai`
  step per RTT-probe event. `num_acked` comes from the ACK PSN delta (tracker ported
  from `device/rp/smartt_reps/`), capped at `max(2·cwnd+2, 256)` so a bad sample cannot
  inject an enormous increase. The delay signal still comes from explicit RTT probes;
  the ACK handler regulates on the most recent probe sample.
- **Trace emission:** formats 16/17 fire from the ACK handler — on every decrease
  decision and on 1-in-64 additive increases (ACKs are far too frequent to trace one
  line each) — so format 19 (probe timestamps) always stands alone. **Field remap:**
  format 16's third field carries `num_acked` here, not `np_turnaround` (an ACK event
  does not decompose a probe); the shared host format string still labels it `np_turn=`.
  Everything else (tsflow packing, field order, format 18 always emitted ungated)
  matches the Swift trace reference in [`../swift/README.md`](../swift/README.md).

## Trace formats

Same IDs and packing as Swift (`tsflow` = timestamp `<<32 | flow_tag` as arg1
everywhere, `rate` last where a format has one); field lists from
[`algo/swift_cx8.h`](algo/swift_cx8.h):

| ID | Fields (arg1…arg5) |
|---|---|
| 15 | `tsflow, init_cwnd, line_mbps, bdp_fxp16, rate` |
| 16 | `tsflow, fabric_rtt, num_acked, target, branch` — the third field is `num_acked`, printed under the shared label `np_turn=` |
| 17 | `tsflow, cwnd, fs_term, can_decrease, rate` |
| 18 | `tsflow, ev_subtype, retx_reset (retx_cnt<<1 \| reset), cwnd, rate` — always emitted, not gated by `trace_on` |
| 19 | `tsflow (ts half = t6/arrival), req_send, req_recv, resp_send, populated` |

Branch/flag encodings, `ev_type` values, and the 16/17 pairing semantics are identical
to the [Swift trace reference](../swift/README.md#trace-format-reference).

## Shared design record

The delay-signal design ("The delay signal"), the live-RTT pacing-denominator choice
and its measured A/B against a constant-`min_rtt` denominator ("Actuating a window on
a rate framework"), and the fabric tuning lessons all live in
[`../swift/README.md`](../swift/README.md) — this variant inherits them unchanged.

## Build / run

```bash
/tmp/build/pcc/doca_pcc --rp-swift-cx8 -d mlx5_0 -l 70    # short flag: -rp-sw8
```

`SWIFT_CX8_DEBUG=1 meson setup ...` force-enables traces from boot; otherwise flip the
runtime `trace_on` PPCC param. The PPCC contract (param/counter indices, defaults,
units) is identical to Swift's except for the algo ID — see
[`algo/swift_cx8_params.h`](algo/swift_cx8_params.h) and the
`swift_cx8_params_dump` host binary.
