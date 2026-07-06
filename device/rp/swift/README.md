# Swift — delay-based window congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
Datacenter", SIGCOMM 2020** (DOI [10.1145/3387514.3406591](https://doi.org/10.1145/3387514.3406591)) —
specifically **Algorithm 1** (§3.2) with the flow-scaled **target-delay** of §3.5.

Full-text mirrors:
[saeed.github.io](https://saeed.github.io/CS8803_DNS_Spring2024/assets/swift_sigcomm20.pdf),
[cs244](https://2022-cs244.github.io/papers/L5-swift.pdf).

Swift is a *window*-based AIMD controller against an *absolute*, flow-scaled target
delay — the simplification of TIMELY (which reacts to the RTT *gradient*). This port
keeps `cwnd` as the control variable and actuates it as a rate (see
[§ Actuating a window on a rate framework](#actuating-a-window-on-a-rate-framework)).

**v1 is fabric-only.** One window (`fcwnd`) driven by the fabric RTT. The endpoint
window (`ecwnd`, paper §3.3) is deferred — see [FIXMEs](#known-unknowns--fixmes).

## File layout

| File | Purpose |
|---|---|
| `algo/swift.h` | Public API: `swift_init`, `swift_algo`, `swift_set_algo_params`; trace gate + format IDs |
| `algo/swift_ctxt.h` | Per-flow state struct, **exactly 48 bytes** (`fcwnd`, `cur_rate`, `t_last_decrease`, `retx_cnt`, RTT-probe abort machinery; `ecwnd`/endpoint reserved) |
| `algo/swift_algo_params.h` | Compiled-in default `#define`s — every value cites a paper section or is marked `FIXME:` |
| `algo/swift_params.h` | The PPCC **contract**: 20-param (19 tuning + 1 diagnostic `trace_on`) / 13-counter table (name, unit, default, min, max), shared by the DPA algorithm and the host `swift_params_dump`. Single source of truth |
| `algo/swift.c` | Algorithm 1 + §3.5 target (see [§ Algorithm 1 mapping](#algorithm-1-mapping)) |
| `rp_swift_dev_main.c` | DPA entry points; slot-15 fallback bypass (see [§ Slot 0 vs slot 15](#slot-0-vs-slot-15)) |

Host-side helper scripts live in [`tools/`](../../../tools) at the application root.

## Build & run

Device-side code is compiled by `dpacc` during `meson setup`, **not** during `ninja`.
After any change to `algo/*.c`, `rp_swift_dev_main.c`, or `build_device_code.sh` you must
re-run `meson setup --wipe` (a bare `ninja` silently uses the stale device binary).

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc --rp-swift -d mlx5_0 -l 70
```

Per-event traces (formats 6, 15–17 and 19) are **compiled in but quiet by default**;
format 18 (loss/NACK) is always emitted — NACKs are rare and a loss event is always
worth a line. Turn the rest on at runtime — no rebuild — by setting the `trace_on` PPCC
param to 1 (back to 0 to silence). To force them on from boot (catch the earliest events), build with
`SWIFT_DEBUG=1`:

```bash
SWIFT_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Actuating a window on a rate framework

BlueField-3's per-flow actuator is `doca_pcc_dev_results_t.rate` (FXP20, `1<<20` = line
rate). The struct also has `credits`/`reload_credits` — the closest thing to a true
window — but on BF3 these are **unbacked** (no field in
`doca_pcc_dev_data_structure_le_bf3.h`, no helpers, no sample usage), so they cannot
actuate a window. Swift therefore keeps `cwnd` as the control variable and maps it to a
rate every event via the **BDP identity**:

```
BDP_pkts   = line_rate_bps × min_rtt_s / (mss_bytes × 8)
rate_fxp20 = clamp( (cwnd_fxp16 << 20) / bdp_fxp16,  min_rate,  MAX_RATE )
```

`cwnd == BDP → line rate`; `cwnd == BDP/2 → half`; **`cwnd < 1 → a deep sub-line-rate
fraction, which is exactly Algorithm 1's `pacing_delay = rtt/cwnd` regime** realized by
the rate limiter. So the one mapping covers both Swift output regimes (windowed for
cwnd≥1, paced for cwnd<1). BDP is computed on-device from the `line_rate_mbps`,
`min_rtt_ns`, and `mss_bytes` params.

> Incast floor caveat: `min_cwnd` defaults to 0.001 pkt (paper §3.4). It is only
> realizable if the FXP20 `min_rate` floor is below the rate that 0.001 pkt maps to;
> otherwise `min_rate` — not 0.001 pkt — is the effective floor. Trace `cwnd`↔`rate`
> (formats 16/17) to confirm they track.

> Pacing-law caveat: the paper's `pacing_delay = rtt/cwnd` (Algorithm 1 lines 29–32)
> uses the *live measured* RTT, so paper-Swift spaces packets further apart as the
> fabric loads. This port's BDP mapping uses the constant `min_rtt` param, so a given
> `cwnd < 1` always paces at no-load spacing — more aggressive than the paper under
> congestion. Faithful near no-load; keep in mind for deep-incast sweeps (an optional
> refinement: scale the mapping by measured RTT when cwnd < 1).

## The flow-scaled target (§3.5) and integer 1/√cwnd

```
target = base_target + num_hops·hop_scale + max(0, min( α/√fcwnd + β, fs_range ))
α = fs_range / (1/√fs_min_cwnd − 1/√fs_max_cwnd),   β = −α / √fs_max_cwnd
```

Since `β = −α·invsqrt(fs_max)`, the scaled term collapses to a **linear interpolation of
`fs_range` over the 1/√cwnd axis** between the two knees — no explicit α/β, no division
of `fs_range` by a tiny denominator:

```
fs_term = fs_range · (invsqrt(cwnd) − invsqrt(fs_max)) / (invsqrt(fs_min) − invsqrt(fs_max))   ∈ [0, fs_range]
```

`cwnd ≤ fs_min_cwnd → fs_range` (max headroom for small/incast flows); `cwnd ≥
fs_max_cwnd → 0`. The DPA has no float/`sqrt`, so `1/√x` uses the FXP16 identity
`2^(−½·log2(x))` via the `__dpa_fxp_log2`/`__dpa_fxp_pow2` intrinsics
(`swift_recip_sqrt_fxp16`). This is the single most precision-sensitive piece; it is
verified indirectly by tracing `fs_term` against the §3.5 curve (`FIXME` in source).

**BF3 note:** `doca_pcc_dev_get_ttl_hoplimit()` returns 0 on BF3, so `num_hops` is a
fixed param and `hop_scale` defaults to 0 (the per-hop term is inert).

## The delay signal

```
total_rtt     = arrival_ts  − req_send_ts      (local-clock duration, t6 − t1)
np_turnaround = resp_send_ts − req_recv_ts      (remote-clock duration, t4 − t2)
fabric_rtt    = total_rtt − np_turnaround        (NIC-to-NIC, queue-aware)
delay         = fabric_rtt
```

Both terms are *durations* each measured within a single clock, so subtracting them is
**sync-free** (no TIME_SYNC needed). `np_turnaround` is the paper's **remote-queuing**
(§3.1 "Measuring Delays": the receiver reflects its NIC-Rx + processing time to the
sender on every ACK; §3.3: fabric-delay = RTT minus the endpoint terms). The residual
local NIC-Rx delay (paper: t6 − t5) stays in `fabric_rtt` — BF3 exposes no t5, and that
is endpoint signal anyway, deferred to `ecwnd` (phase 2).

**A probe response without responder timestamps is not a delay sample.** The paper
assumes stamping, Swift-capable endpoints and defines no fallback for their absence, so
neither does this implementation: if `t2`/`t4` are missing (0) or inconsistent
(`np_turnaround ≥ total_rtt`), the event is **dropped** — no cwnd/rate change, the
`rtt_dropped_unstamped` counter increments, and the next probe is still requested. A
non-stamping responder therefore leaves Swift *visibly inert* (rate pinned at its
initial value, `ai`/`md` counters frozen at 0, the drop counter climbing at probe
cadence) rather than silently regulating on total RTT including the remote turnaround.
Measured on this testbed (2026-07-06): a ConnectX-5 Ex responder (`alveo-u55c-03`)
reflects probes but **never stamps** — 95/95 probes arrived `populated=0` in format 19 —
so Swift against that receiver exercises exactly this drop path by design. Swift needs a
responder that stamps RTT probes, e.g. a BlueField running an NP PCC app
(`doca_pcc --np-nic-telemetry`).

**Serialization** is *not* subtracted: per the design decision (and matching TIMELY,
which subtracts none and folds everything into its empirically-retuned thresholds) it is
absorbed into `base_target`. For fixed-MTU RoCE this is algebraically identical to an
explicit subtraction. A per-segment subtraction would only matter for variable segment
sizes, for which BF3 exposes no clean per-event byte source (`FIXME` in source).

## Tuning parameters for your fabric

The paper gives formula structure and parameter *names* but **no canonical numeric
tuning table**. The only concrete paper values: `min_cwnd = 0.001` pkt (§3.4);
`base_target ≈ 25 µs` (a stack-limited test) / 50 µs fabric (a storage test). `ai=1`,
`β≈0.8`, `max_mdf=0.5` are typical/community values. **Every default is a seed to sweep.**

**Critical:** like TIMELY, `base_target` (paper ~25 µs, Google's fabric) must be retuned
— idle fabric RTT here measured ~3–4 µs, so a 25 µs target would never trigger a
decrease. Default seed is **10 µs**; set it from a no-load `fabric_rtt` trace (format 16).

### Runtime override via PPCC (no recompile)

The 19 tuning params are exposed through the standard PPCC register interface; a host
orchestrator pushes a new vector between runs without rebuilding the DPA. The Mac-side
[`incast-bench`](../../../../incast-bench) orchestrator dumps the contract via
`swift_params_dump`, pushes a vector via `mlxreg cmd_type=0xB`, and scrapes the 13
counters via `cmd_type=0xC`/`0xD`. The diagnostic `trace_on` toggle (last param index)
rides the same channel; sweeps should leave it at 0.

**Target `local_port=1`, not `mlx5_0`'s data port:** per [§ Slot 0 vs slot
15](#slot-0-vs-slot-15) below, the device dispatcher pins params/counters to DPA
`port_num=0` / `algo_slot=0` regardless of where traffic actually lands, so
`mlxreg ... local_port=1 ...` (which the firmware maps to DPA port 0) is the address
that reaches the running algorithm — pushing to any other `local_port` silently no-ops
(read-back via `cmd_type=0xA` looks fine; the algorithm never sees it). A set targeting
`algo_slot != 0` now returns `DOCA_PCC_DEV_STATUS_FAIL` rather than silently no-op'ing.

## Slot 0 vs slot 15

`doca_pcc_dev_user_algo` **ignores the negotiated `algo_slot` and always reads slot 0's
params/counters** — Swift registers on slot 0 (`algo_idx=0`), but `ib_write_bw` flows
that do not negotiate via ECE land on **slot 15** and still run Swift. The practical
consequence: a param vector pushed via PPCC `algo_slot=0` governs the slot-15 benchmark
traffic, and the slot-0 counters aggregate it. (A future multi-algorithm binary must
instead dispatch on `attr->algo_slot`.)

For the same reason the dispatcher pins to **port 0** rather than the event's port
(`rp_swift_dev_main.c`). Param/counter arrays are per `(port, slot)`: the orchestrator
pushes params and scrapes counters over mlxreg PPCC with `local_port=1`, which the
firmware maps to **DPA `port_num=0`** (the `doca_pcc_dev_user_set_algo_params` callback
fires with `port_num=0`), but RoCE traffic on `mlx5_0` reaches the algo on **DPA
`port_num=1`** (format-6 events show `port=1`). Fetching params/counters for the *event*
port would read an array the push never touched — a pushed param would read back fine via
`cmd_type=0xA` yet never reach the running algorithm, and counter increments would strand
on a port the orchestrator never scrapes (root-caused on TIMELY, confirmed on BF3
hardware). Pinning to port 0 keeps the algo, the param push, and the counter scrape on
one array. Revisit if a build ever drives multiple physical ports.

## Trace format reference

| ID | Source | Field meanings |
|---|---|---|
| 0–1 | DOCA infra | `user init` / `user algo` (every algo) |
| 2–5 | NP-side | only fire under a `--np-*` app — ignore on RP/Swift |
| 6 | per-event summary (`rp_swift_dev_main.c`) | `ev_type` (table below); `flow_tag`; `port`; `algo_slot`; `rate` (FXP20 after algo) |
| 7–14 | reserved for DCQCN (7–10) / TIMELY (11/12/14, 13 retired) | placeholders for the eventual merged build — never emitted on the swift branch |
| 15 | Swift `new_flow` | `init_cwnd` (FXP16 pkts); `rate` (FXP20); `ts`; `line_mbps`; `bdp_fxp16` |
| 16 | Swift `rtt` (delay side) | `fabric_rtt` (ns); `np_turn` (ns); `delay_minus_target` (**signed** ns); `target` (ns); `branch` (table below) |
| 17 | Swift `rtt` (window side) | `cwnd_prev`, `cwnd` (FXP16 pkts); `rate` (FXP20); `fs_term` (ns, = target − base); `can_decr` |
| 18 | Swift `loss` (NACK) — **always emitted**, not gated by `trace_on` (NACKs are rare) | `ev_subtype` (1=RNR, 2=OOS, 3=DUP_READ; measured: ramp NACKs are OOS=2); `retx_cnt`; `cwnd` (FXP16); `rate` (FXP20); `reset` (1 = collapsed to min_cwnd) |
| 19 | Swift `rtt` raw timestamps | `t1_req_send`, `t2_req_recv`, `t4_resp_send`, `t6_arrival` (raw ns, wrapping u32); `populated` (bit0 = t2≠0, bit1 = t4≠0; **3 = receiver stamped both**, else the sample is **dropped** and only `rtt_dropped_unstamped` moves) |

Formats 16 and 17 are two halves of one RTT decision (split because `trace_5` carries
only five args); join them by order. Format 19 precedes them for the same RTT event.
**A dropped sample emits format 19 alone** — no 16/17 pair follows it. When pairing
16/17 by order, a 19-line with no pair after it marks a drop, not a missing trace.

### `ev_type`

| ev_type | Name | Meaning |
|---|---|---|
| 2 | `CNP` | RoCE CNP — **ignored** (Swift is delay-based; paper §3.7 isolates via QoS) |
| 3 | `TX`  | TX burst ended — maintains the RTT-probe abort state machine |
| 4 | `ACK` | RoCE ACK — resets `retx_cnt` (Algorithm 1 line 5); no window/rate change |
| 5 | `NACK`| RoCE NACK — the **loss/RTO** signal (format 18) |
| 6 | `RTT` | RTT probe response — **Swift's main signal** (Algorithm 1) |

### `branch` (format 16; low nibble = Algorithm-1 path, high bits OR-ed in)

| branch | Algorithm 1 | Action |
|---|---|---|
| `0x00` `AI` | `delay < target` | `cwnd += ai` (additive increase) |
| `0x01` `MD` | `delay ≥ target`, `can_decrease` | `cwnd ← max(1−β·(delay−target)/delay, 1−max_mdf)·cwnd` |
| `0x02` `MD_BLOCKED` | `delay ≥ target`, but once-per-RTT guard blocked it | no change |
| `0x80` bit | — | actuated rate clamped to `min_rate` or `MAX_RATE` |
| `0x40` bit | — | `cwnd` ended `< 1` pkt (pacing regime) |
| `0x20` bit | — | flow-scale term saturated (0 or `fs_range`) |

## Algorithm 1 mapping

```
# precondition: the probe response carries responder timestamps t2/t4 (§3.1).
#   Unstamped/inconsistent → drop the sample: rtt_dropped_unstamped++, rate and cwnd
#   unchanged, next probe re-requested. The paper defines no fallback, so none exists.
cwnd_prev    ← cwnd
can_decrease ← (now − t_last_decrease ≥ rtt)              [§3.2: MD at most once/RTT]
target       ← TargetDelay(cwnd)                         [§3.5, swift_flow_scaled_target]

if delay < target:                                       (SWIFT_BRANCH_AI)
    cwnd += ai          # one +ai step per RTT event — see AI cadence note below
else if can_decrease:                                    (SWIFT_BRANCH_MD)
    cwnd ← max(1 − β·(delay−target)/delay, 1 − max_mdf) · cwnd
# else: MD_BLOCKED (no change)

cwnd ← clamp(min_cwnd, cwnd, max_cwnd)
if cwnd ≤ cwnd_prev: t_last_decrease ← now
rate ← cwnd_to_rate(cwnd)                                # BDP; realizes pacing for cwnd<1

# On ACK (Algorithm 1 line 5): retx_cnt ← 0 — data forward progress; no window change
#   (delay is probe-driven on BF3). Usable RTT samples also reset it; dropped ones don't.
# On NACK (loss, §3.6): retx_cnt++; if ≥ RETX_RESET_THRESHOLD → cwnd = min_cwnd,
#   else (once/RTT) cwnd ·= (1 − max_mdf).
```

**AI cadence (FIXME in source):** the paper's AI nets `+ai` per RTT via
`(ai/cwnd)·num_acked`. DOCA delivers discrete RTT-probe events (~107 µs apart measured
on this fabric, ≫ 1 RTT) with
no `num_acked`, so — matching TIMELY's validated choice on this exact fabric — Swift
applies **one `+ai` step per RTT event** rather than scaling by `elapsed/RTT` (which
would multiply `ai` by ~1000s and explode `cwnd`). Tune convergence via `ai` / probe
cadence.

## Known unknowns / FIXMEs

```bash
grep -rn "FIXME" device/rp/swift/
```

Highlights:
- **Integer 1/√cwnd:** precision/sign behaviour of `__dpa_fxp_log2`/`__dpa_fxp_pow2` for
  sub-1 inputs / negative exponents is unverified on silicon — validated by tracing `fs_term`.
- **cwnd→rate BDP mapping:** the approximation most likely wrong on first pass — traced side-by-side.
- **Flow-scale constants** (`fs_range`, `fs_min_cwnd`, `fs_max_cwnd`), **`md_beta`,
  `max_mdf`, `max_cwnd`, `retx_reset_threshold`:** paper gives no values — seeded, sweep.
- **Endpoint window (`ecwnd`, §3.3):** deferred to phase 2 (params reserved, inert).
- **NACK = loss:** every NACK is folded into the conservative RTO path. **Measured
  2026-07-06: `ev_subtype` is populated** — ramp NACKs are OOS (=2; enum in
  `doca_pcc_dev.h`: 1=RNR, 2=OOS, 3=DUP_READ) — so the fast-recovery split
  (Algorithm 1 lines 22–25) is implementable: OOS/DUP_READ ≈ SACK-class, RNR ≈
  receiver backpressure. The enum has no timeout subtype (a true RTO never surfaces
  as a NACK event), which reframes what the `retx_reset_thresh` collapse guards.
  Unimplemented pending a design decision.
- **RTO timing (§3.6):** the paper's per-flow EWMA-based retransmission timeout is loss
  *detection*, which is firmware-owned on BF3 (the RP only receives already-detected
  NACK events). Intentionally out of RP scope; only the *reaction* is implemented.
- **`num_acked` / once-per-RTT cadence:** simplified to one step per event (see above).
- **Serialization:** folded into `base_target` rather than subtracted (matches TIMELY; PDF §3.5).

## Hardware / branch context

- Targets BlueField-3 (DPA) at `alveo-u50d-01`. algo_id `0xBFFC`.
- Independent sibling of the `timely` (`0xBFFD`) and `dcqcn` (`0xBFFE`) branches — no
  shared algorithm code (disjoint `device/rp/*` trees). The build/host glue is additive
  so all three can merge into one runtime-selectable build; trace format IDs 6–14 mirror
  the timely/dcqcn reservations so the merge is conflict-free, with 15–19 owned by Swift.
- The default app in `host/pcc.c` is **unchanged from main** — Swift is selected at
  runtime via `--rp-swift` / `-rp-sw`.
- One ACTIVE PCC process per BlueField (firmware HA): you cannot run Swift and another
  PCC algorithm actively on the same card simultaneously.
