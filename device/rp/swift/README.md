# Swift: delay-based window congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the
Datacenter", SIGCOMM 2020** (DOI [10.1145/3387514.3406591](https://doi.org/10.1145/3387514.3406591)),
specifically **Algorithm 1** (§3.2) with the flow-scaled **target-delay** of §3.5.

Full-text mirrors:
[saeed.github.io](https://saeed.github.io/CS8803_DNS_Spring2024/assets/swift_sigcomm20.pdf),
[cs244](https://2022-cs244.github.io/papers/L5-swift.pdf).

Swift is a *window*-based AIMD controller against an *absolute*, flow-scaled target
delay, the simplification of TIMELY (which reacts to the RTT *gradient*). This port
keeps `cwnd` as the control variable and actuates it as a rate (see
[§ Actuating a window on a rate framework](#actuating-a-window-on-a-rate-framework)).

**v1 is fabric-only.** One window (`fcwnd`) driven by the fabric RTT. The endpoint
window (`ecwnd`, paper §3.3) is deferred; see [FIXMEs](#known-unknowns--fixmes).

## File layout

| File | Purpose |
|---|---|
| `algo/swift.h` | Public API: `swift_init`, `swift_algo`, `swift_set_algo_params`; trace gate + format IDs |
| `algo/swift_ctxt.h` | Per-flow state struct, **exactly 48 bytes** (`fcwnd`, `cur_rate`, `t_last_decrease`, `retx_cnt`, RTT-probe abort machinery; `ecwnd`/endpoint reserved) |
| `algo/swift_algo_params.h` | Compiled-in default `#define`s; every value cites a paper section, unbacked tuning seeds are recorded in this README |
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

Per-event traces (formats 6 and 15-19) are **compiled in but quiet by default**.
Turn them on at runtime, without a rebuild, by setting the `trace_on` PPCC
param to 1 (back to 0 to silence). To force them on from boot (catch the earliest events), build with
`SWIFT_DEBUG=1`:

```bash
SWIFT_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Actuating a window on a rate framework

BlueField-3's per-flow actuator is `doca_pcc_dev_results_t.rate` (FXP20, `1<<20` = line
rate). The struct also has `credits`/`reload_credits`, the closest thing to a true
window, but these are **dead fields**: disassembling the shipped device library
(DOCA 2.10, `libdoca_pcc_dev.a` → `pcc_rp_dev_rp.<chip>.o` LLVM bitcode) shows the
framework glue that runs after `doca_pcc_dev_user_algo` loads only `rate` (offset 0,
passed through `llvm.riscv.nv.rcp`: HW takes the reciprocal, a period) and `rtt_req`
(offset 4), packs them into a single 64-bit DPA-outbox doorbell store, and never reads
offsets 12/16 (`credits`/`reload_credits`), on BF3, CX7, *or* CX8 (CX8 additionally
reads `probe_type_slot`). NVIDIA's own built-in DCQCN never writes them either. Swift
therefore keeps `cwnd` as the control variable and maps it to a rate every event via
the **pacing law**: the BDP identity evaluated at the *measured* fabric RTT:

```
pacing_rtt = clamp(last_fabric_rtt, min_rtt, 100 ms)
BDP_pkts   = line_rate_bps × pacing_rtt_s / (mss_bytes × 8)
rate_fxp20 = clamp( (cwnd_fxp16 << 20) / bdp_fxp16,  min_rate,  MAX_RATE )
```

`cwnd == BDP → line rate`; `cwnd == BDP/2 → half`; **`cwnd < 1 → a deep sub-line-rate
fraction, which is exactly Algorithm 1's `pacing_delay = rtt/cwnd` regime** realized by
the rate limiter. So the one mapping covers both Swift output regimes (windowed for
cwnd≥1, paced for cwnd<1).

Dividing by the **live** RTT (the paper's actual law, Algorithm 1 lines 29-32) is what
preserves window semantics on a rate actuator: inflight = rate × RTT (Little's law), so
`rate = cwnd·MSS·8/rtt` holds inflight ≈ cwnd as the fabric loads. A constant
denominator loses that: at the RTTs measured on this fabric (~15 µs loaded vs 3.8 µs
idle) the old constant-`min_rtt` mapping admits ≈ 4× cwnd of inflight under load,
leaving Swift's delay loop to claw the excess back indirectly. The denominator is the
`last_fabric_rtt` ctxt field, the same instantaneous sample that drives AIMD, updated
on every usable RTT event; the `min_rtt` floor means the live mapping is never *faster*
than the constant one at the same cwnd (a sub-`min_rtt` sample is noise, not headroom),
and the 100 ms cap only guards uint64 arithmetic. Before the first sample (and on the
new-flow path) it falls back to `min_rtt`.

The claim was verified on hardware (2026-08-12, 2:1 incast: BF3 Swift sender +
uncontrolled CX-5 sender → BF3 receiver, 3×20 s per arm, via a temporary
`pacing_rtt_on` toggle removed after the comparison). Live-RTT vs constant-`min_rtt`,
per run: over-target RTT samples 24.7% vs 33.9%, receiver ECN marks ~2.5 k vs ~13.7 k
(5.4×), CNPs ~1.8 k vs ~10 k, PFC pauses at the sender 40/0/0 vs ~130 every run,
`cwnd_below_one` 0 vs ~50, for 0.7 Gb/s (1.6%) less of the receiver against the
uncontrolled competitor, at identical ~97.8 Gb/s aggregate. The constant mapping holds
a deeper queue for a sliver of share; the live mapping is the law and has no toggle.
(One counter reads high by design: `rate_clamped_max`; the live mapping runs a larger
equilibrium cwnd, so a near-`min_rtt` sample momentarily maps it above line rate and
the clamp catches it.)

> Incast floor caveat: `min_cwnd` defaults to 0.001 pkt (paper §3.4). It is only
> realizable if the FXP20 `min_rate` floor is below the rate that 0.001 pkt maps to;
> otherwise `min_rate`, not 0.001 pkt, is the effective floor. Trace `cwnd`↔`rate`
> (formats 16/17) to confirm they track.

## The flow-scaled target (§3.5) and integer 1/√cwnd

```
target = base_target + num_hops·hop_scale + max(0, min( α/√fcwnd + β, fs_range ))
α = fs_range / (1/√fs_min_cwnd − 1/√fs_max_cwnd),   β = −α / √fs_max_cwnd
```

Since `β = −α·invsqrt(fs_max)`, the scaled term collapses to a **linear interpolation of
`fs_range` over the 1/√cwnd axis** between the two knees: no explicit α/β, no division
of `fs_range` by a tiny denominator:

```
fs_term = fs_range · (invsqrt(cwnd) − invsqrt(fs_max)) / (invsqrt(fs_min) − invsqrt(fs_max))   ∈ [0, fs_range]
```

`cwnd ≤ fs_min_cwnd → fs_range` (max headroom for small/incast flows); `cwnd ≥
fs_max_cwnd → 0`. The DPA has no float/`sqrt`, so `1/√x` uses the FXP16 identity
`2^(−½·log2(x))` via the `__dpa_fxp_log2`/`__dpa_fxp_pow2` intrinsics
(`swift_recip_sqrt_fxp16`). This is the single most precision-sensitive piece, verified by
tracing `fs_term` (format 17) against the §3.5 curve: on silicon (2026-08-17, BF3,
DOCA 2.10) 610 traced pairs match the ideal float curve within 19 ns of the 15 µs
`fs_range` (0.13%) over `invsqrt` inputs 0.1-1024 pkt, including sub-unity fractional
`cwnd`.

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
local NIC-Rx delay (paper: t6 − t5) stays in `fabric_rtt`: BF3 exposes no t5, and that
is endpoint signal anyway, deferred to `ecwnd` (phase 2).

**A probe response without responder timestamps is not a delay sample.** The paper
assumes stamping, Swift-capable endpoints and defines no fallback for their absence, so
neither does this implementation: if `t2`/`t4` are missing (0) or inconsistent
(`np_turnaround ≥ total_rtt`), the event is **dropped**: no cwnd/rate change, the
`rtt_dropped_unstamped` counter increments, and the next probe is still requested. A
non-stamping responder therefore leaves Swift *visibly inert* (rate pinned at its
initial value, `ai`/`md` counters frozen at 0, the drop counter climbing at probe
cadence) rather than silently regulating on total RTT including the remote turnaround.
Measured on this testbed (2026-07-06): a ConnectX-5 Ex responder (`hacc-u55c-03`)
reflects probes but **never stamps** (95/95 probes arrived `populated=0` in format 19),
so Swift against that receiver exercises exactly this drop path by design.

Responder support matrix (measured 2026-07-08; NVIDIA publishes none; the DOCA PCC
guide only states that for CCMAD, "responses are handled by the remote node's hardware"
when no NP process is set, and that non-CCMAD probe formats always require a remote NP
process):
- **BlueField-3** (`sgs-bluefield-02`): stamps t2/t4 on every probe, in NIC firmware, with
  **no PCC process running at all**; no NP app needed for RP-side Swift.
- **ConnectX-6** (MT4123 `[15b3:101b]`, fw 20.31.1014, `alveo-v80-01`): stamps **only
  when idle**: 14/15 `populated=3` on a broken/idle flow (`np_turn` ≈ 370 ns, fabric
  ≈ 3.8 µs), but **under real load it answers every probe unstamped**: at 34 Gb/s,
  464,092/464,092 probes dropped (`populated=0`, probe RTT p50 ≈ 33 µs), `ai=md=0`,
  bandwidth frozen at the 2-NACK ratchet plateau (≈34.1 Gb/s): Swift designed-inert,
  exactly as against CX-5. **Not viable as a Swift responder**: the signal disappears
  precisely when congestion exists. (Untested lever: newer CX-6 firmware.)
- **ConnectX-5 Ex** (`hacc-u55c-03`): never stamps (above).

**Serialization** is *not* subtracted: per the design decision (and matching TIMELY,
which subtracts none and folds everything into its empirically-retuned thresholds) it is
absorbed into `base_target`. For fixed-MTU RoCE this is algebraically identical to an
explicit subtraction. A per-segment subtraction would only matter for variable segment
sizes, for which BF3 exposes no clean per-event byte source.

## The endpoint window (§3.3): measured dead on BF3

Swift regulates `min(fcwnd, ecwnd)`, where `ecwnd` reacts to *endpoint* delay: the
receiver's NIC-Rx queueing and host processing time (§3.3). Phase 2 of this
implementation was to drive `ecwnd` from `np_turnaround` (`t4 - t2`), the only
endpoint-side duration BF3 reflects back to the sender. A characterization run on
2026-08-20 shows that quantity does not respond to load at all, so `ecwnd` stays
registered but inert **by design**, not as a deferral.

### Setup

Sender `sgs-bluefield-01` running `--rp-swift` (algo_id `0xBFFC`, confirmed via PPCC
`cmd_type=0x0` before collecting), receiver `sgs-bluefield-02` running **no PCC process**
(BF3 firmware answers the probes), background sender `hacc-u55c-03` also running no
PCC. 40 s per arm, `ib_write_bw -d mlx5_0 -R -x 3 -F -s 16384`, numbers read from
format 16 with `trace_on` pushed at runtime.

| Arm | Load | achieved | probes | unstamped | np_turn p50 | np_turn p99 | fabric_rtt p50 | fabric_rtt p99 |
|---|---|---|---|---|---|---|---|---|
| A | 1:1, SW-paced 10 Gb/s | 10.00 G | 923 | 0 | 106 | 106 | 3680 | 3712 |
| B | 1:1, line rate | 97.59 G | 652 | 0 | 106 | 106 | 3694 | 3874 |
| C | 2:1, receiver-bound | 0.04 G (bg 97.72 G) | 1172 | 0 | 106 | 106 | 44226 | 49032 |

All times in ns. Full `np_turn` spread: A 104 to 230, B 104 to 108, C 104 to 108.

### Result

`np_turnaround` is flat at 106 ns in every arm, and the loaded spread (104 to 108) is
*narrower* than the idle spread (104 to 230), so there is not even a distribution shift
to argue about. Across the same runs `fabric_rtt` p50 moves from 3680 ns to 44226 ns, a
factor of 12: that is the proof that arm C genuinely oversubscribed the receiver port
and that the load was real. The congestion signal lives entirely in the fabric term.

`np_turnaround` on BF3 is therefore a fixed firmware responder cost of about 106 ns,
roughly two ticks of the nanosecond timer, generated in the NIC before any
receiver-side queue exists. It is load-invariant by construction, not by coincidence,
and it is not a queueing measurement.

### Why there is no second source

The other route, a receiver-side NP application writing real telemetry into the probe
response, is structurally blocked. `union mlnx_cc_event_spec_attr_t` in
`doca_pcc_dev_data_structure_le_bf3.h` overlays `rtt_tstamp` (t1, t2, t4) and
`reserved_at_0[16]`, and `doca_pcc_dev_get_rtt_raw_data()` returns that same array: NP
user data and the firmware t2/t4 stamps occupy the same 12 bytes. An NP app that
supplies an endpoint signal destroys the fabric-delay signal in the process, because
the stock NP handler writes its payload into exactly the t1/t2/t4 slots. There is no
configuration in which both are available.

### Consequence

`ecwnd` and `endpoint_delay_ewma` stay in `cc_ctxt_swift_t`, and params 17
(`endpoint_target_ns`) and 18 (`endpoint_ewma`) stay registered so the index map does
not move, but the algorithm does not read them. Reviving `ecwnd` needs a new endpoint
signal: a NIC generation that reflects real receiver queue occupancy, or a probe format
that carries NP data *alongside* the stamps rather than on top of them.

Recorded because arm C surfaced it, and separate from the endpoint question: under an
uncontrolled 2:1 competitor the Swift flow collapsed to 0.04 Gb/s while the unmanaged
background flow took 97.72 Gb/s, which is a Swift v1 fairness behaviour against a
non-responsive peer.

## Tuning parameters for your fabric

The paper gives formula structure and parameter *names* but **no canonical numeric
tuning table**. The only concrete paper values: `min_cwnd = 0.001` pkt (§3.4);
`base_target ≈ 25 µs` (a stack-limited test) / 50 µs fabric (a storage test). `ai=1`,
`β≈0.8`, `max_mdf=0.5` are typical/community values. **Every default is a seed to sweep.**

**Critical:** like TIMELY, `base_target` (paper ~25 µs, Google's fabric) must be retuned:
idle fabric RTT here measured ~3-4 µs, so a 25 µs target would never trigger a
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
that reaches the running algorithm; pushing to any other `local_port` silently no-ops
(read-back via `cmd_type=0xA` looks fine; the algorithm never sees it). A set targeting
`algo_slot != 0` now returns `DOCA_PCC_DEV_STATUS_FAIL` rather than silently no-op'ing.

## Slot 0 vs slot 15

`doca_pcc_dev_user_algo` **ignores the negotiated `algo_slot` and always reads slot 0's
params/counters**: Swift registers on slot 0 (`algo_idx=0`), but `ib_write_bw` flows
that do not negotiate via ECE land on **slot 15** and still run Swift. The practical
consequence: a param vector pushed via PPCC `algo_slot=0` governs the slot-15 benchmark
traffic, and the slot-0 counters aggregate it. (The `--rp-multi` app ignores
`attr->algo_slot` too; it picks the algorithm from its own mux parameter instead, see
[multi](../multi/README.md).)

For the same reason the dispatcher pins to **port 0** rather than the event's port
(`rp_swift_dev_main.c`). Param/counter arrays are per `(port, slot)`: the orchestrator
pushes params and scrapes counters over mlxreg PPCC with `local_port=1`, which the
firmware maps to **DPA `port_num=0`** (the `doca_pcc_dev_user_set_algo_params` callback
fires with `port_num=0`), but RoCE traffic on `mlx5_0` reaches the algo on **DPA
`port_num=1`** (format-6 events show `port=1`). Fetching params/counters for the *event*
port would read an array the push never touched: a pushed param would read back fine via
`cmd_type=0xA` yet never reach the running algorithm, and counter increments would strand
on a port the orchestrator never scrapes (root-caused on TIMELY, confirmed on BF3
hardware). Pinning to port 0 keeps the algo, the param push, and the counter scrape on
one array. Revisit if a build ever drives multiple physical ports.

## Trace format reference

Every Swift trace line (formats 6, 15-19) carries a packed `tsflow` as its arg1, printed
`tsflow=0x%lx`, so "which flow, when" sits at one fixed position in every format.
Packing (`SWIFT_TRACE_TSFLOW` in `algo/swift.h`):

```
bits 63:32 = ts_ns    (the uint32_t doca_pcc_dev_get_timestamp() value; wraps ~4.295s)
bits 31:0  = flow_tag (the uint32_t doca_pcc_dev_get_flowtag() value)
```

Printed fields are otherwise contiguous in emission order, with `rate` last wherever a
format has one. Two fields are deliberately not on the wire because they are derivable
host-side: format 16 carries no `delay_minus_target` (it is `fabric_rtt − target`) and
format 17 no `cwnd_prev` (it is the previous format-17 line for the same flow). Lines
emitted for the *same* RTT event (formats 19, 16, 17) carry an identical `tsflow`, so, in
addition to log order, a reader can group them by that value.

| ID | Source | Field meanings |
|---|---|---|
| 0-1 | DOCA infra | `user init` / `user algo` (every algo) |
| 2-5 | NP-side | only fire under a `--np-*` app; ignore on RP/Swift |
| 6 | per-event summary (`rp_swift_dev_main.c`) | `tsflow`; `ev_type` (table below); `port`; `algo_slot`; `rate` (FXP20 after algo) |
| 7-14 | DCQCN (7-10) / TIMELY (11/12/14, 13 retired) | owned by the other algorithms; never emitted by Swift |
| 15 | Swift `new_flow` | `tsflow`; `init_cwnd` (FXP16 pkts); `line_mbps`; `bdp_fxp16`; `rate` (FXP20) |
| 16 | Swift `rtt` (delay side) | `tsflow`; `fabric_rtt` (ns); `np_turn` (ns); `target` (ns); `branch` (table below) |
| 17 | Swift `rtt` (window side) | `tsflow`; `cwnd` (FXP16 pkts); `fs_term` (ns, = target − base); `can_decr`; `rate` (FXP20) |
| 18 | Swift `loss` (NACK) | `tsflow`; `ev_subtype` (1=RNR, 2=OOS, 3=DUP_READ; measured: ramp NACKs are OOS=2); `retx_reset` (bits 31:1 = `retx_cnt`, bit0 = `reset`, 1 = collapsed to min_cwnd); `cwnd` (FXP16); `rate` (FXP20) |
| 19 | Swift `rtt` raw timestamps | `tsflow` (ts half = `t6`/arrival); `t1_req_send`, `t2_req_recv`, `t4_resp_send` (raw ns, wrapping u32); `populated` (bit0 = t2≠0, bit1 = t4≠0; **3 = receiver stamped both**, else the sample is **dropped** and only `rtt_dropped_unstamped` moves) |

Formats 16 and 17 are two halves of one RTT decision (split because `trace_5` carries
only five args); join them by order or by identical `tsflow`. Format 19 precedes them
for the same RTT event. **A dropped sample emits format 19 alone**: no 16/17 pair
follows it, though it still carries that event's `tsflow`. When pairing 16/17 by order,
a 19-line with no pair after it marks a drop, not a missing trace.

### `ev_type`

| ev_type | Name | Meaning |
|---|---|---|
| 2 | `CNP` | RoCE CNP: **masked out** in `user_init` (`disable_event_bitmask`), never reaches the DPA (Swift is delay-based; paper §3.7 isolates via QoS) |
| 3 | `TX`  | TX burst ended: maintains the RTT-probe abort state machine |
| 4 | `ACK` | RoCE ACK: resets `retx_cnt` (Algorithm 1 line 5); no window/rate change |
| 5 | `NACK`| RoCE NACK: the OOS subtype is the **loss/RTO** signal; RNR/DUP_READ leave state untouched (format 18 traces all subtypes) |
| 6 | `RTT` | RTT probe response: **Swift's main signal** (Algorithm 1) |

### `branch` (format 16; low nibble = Algorithm-1 path, high bits OR-ed in)

| branch | Algorithm 1 | Action |
|---|---|---|
| `0x00` `AI` | `delay < target` | `cwnd += ai` (additive increase) |
| `0x01` `MD` | `delay ≥ target`, `can_decrease` | `cwnd ← max(1−β·(delay−target)/delay, 1−max_mdf)·cwnd` |
| `0x02` `MD_BLOCKED` | `delay ≥ target`, but once-per-RTT guard blocked it | no change |
| `0x80` bit | - | actuated rate clamped to `min_rate` or `MAX_RATE` |
| `0x40` bit | - | `cwnd` ended `< 1` pkt (pacing regime) |
| `0x20` bit | - | flow-scale term saturated (0 or `fs_range`) |

## Algorithm 1 mapping

```
# precondition: the probe response carries responder timestamps t2/t4 (§3.1).
#   Unstamped/inconsistent → drop the sample: rtt_dropped_unstamped++, rate and cwnd
#   unchanged, next probe re-requested. The paper defines no fallback, so none exists.
cwnd_prev    ← cwnd
can_decrease ← (now − t_last_decrease ≥ rtt)              [§3.2: MD at most once/RTT]
target       ← TargetDelay(cwnd)                         [§3.5, swift_flow_scaled_target]

if delay < target:                                       (SWIFT_BRANCH_AI)
    cwnd += ai          # one +ai step per RTT event; see AI cadence note below
else if can_decrease:                                    (SWIFT_BRANCH_MD)
    cwnd ← max(1 − β·(delay−target)/delay, 1 − max_mdf) · cwnd
# else: MD_BLOCKED (no change)

cwnd ← clamp(min_cwnd, cwnd, max_cwnd)
if cwnd ≤ cwnd_prev: t_last_decrease ← now
rate ← cwnd_to_rate(cwnd, pacing_rtt)     # cwnd/BDP(live RTT); realizes pacing for cwnd<1

# On ACK (Algorithm 1 line 5): retx_cnt ← 0 (data forward progress); no window change
#   (delay is probe-driven on BF3). RTT samples do NOT reset it: a returning probe
#   proves only that the probe survived, not that data was delivered.
# On OOS NACK (loss, §3.6): retx_cnt++; if ≥ RETX_RESET_THRESHOLD → cwnd = min_cwnd,
#   else (once/RTT) cwnd ·= (1 − max_mdf). Other NACK subtypes: no-op, return cur_rate.
```

**AI cadence:** the paper's AI nets `+ai` per RTT via
`(ai/cwnd)·num_acked`. DOCA delivers discrete RTT-probe events (~107 µs apart measured
on this fabric, ≫ 1 RTT) with
no `num_acked`, so, matching TIMELY's validated choice on this exact fabric, Swift
applies **one `+ai` step per RTT event** rather than scaling by `elapsed/RTT` (which
would multiply `ai` by ~1000s and explode `cwnd`). Tune convergence via `ai` / probe
cadence.

## Known unknowns / FIXMEs

Open choices are recorded here rather than as FIXME comments in the source.

Highlights:
- **Integer 1/√cwnd:** no longer open: the precision/sign behaviour of
  `__dpa_fxp_log2`/`__dpa_fxp_pow2` for sub-1 inputs and negative exponents was validated
  on silicon (2026-08-17, see above); a Newton step or a cwnd-bucketed LUT remains the
  fallback if a future input range misbehaves.
- **Flow-scale constants** (`fs_range`, `fs_min_cwnd`, `fs_max_cwnd`), **`md_beta`,
  `max_mdf`, `max_cwnd`, `retx_reset_threshold`:** paper gives no values; seeded, sweep.
- **`max_cwnd` / `init_cwnd` seeds:** the paper gives neither value; 64 pkts and 16 pkts
  (about one BDP) were chosen here.
- **`min_rate`:** paper defines no rate floor; reuses the `1<<6` FXP20 floor from the
  TIMELY/DCQCN implementations.
- **Endpoint window (`ecwnd`, §3.3):** closed as a negative result, not an open item:
  the only endpoint signal BF3 exposes (`np_turnaround`) is load-invariant, measured
  2026-08-20 (see "The endpoint window (§3.3): measured dead on BF3"). Params reserved,
  inert by design.
- **OOS NACK = loss:** closed: only OOS NACKs (`ev_subtype` 2; enum in
  `doca_pcc_dev.h`: 1=RNR, 2=OOS, 3=DUP_READ) enter the conservative RTO path;
  RNR and DUP_READ report no loss, so the handler leaves the flow state untouched
  and returns the current rate (format 18 still traces every subtype). Measured
  2026-07-06: `ev_subtype` is populated, ramp NACKs arrive as OOS. The paper's
  fast-recovery/RTO split itself remains unbuilt: the enum has no timeout subtype
  (a true RTO never surfaces as a NACK event), which reframes what the
  `retx_reset_thresh` collapse guards.
- **RTO timing (§3.6):** the paper's per-flow EWMA-based retransmission timeout is loss
  *detection*, which is firmware-owned on BF3 (the RP only receives already-detected
  NACK events). Intentionally out of RP scope; only the *reaction* is implemented.
- **`num_acked` / once-per-RTT cadence:** simplified to one step per event (see above).
- **Serialization:** folded into `base_target` rather than subtracted (matches TIMELY; paper §3.5).

## Hardware / branch context

- Targets BlueField-3 (DPA) at `sgs-bluefield-01`. algo_id `0xBFFC`.
- Ships in the same `doca_pcc` binary as TIMELY (`0xBFFD`), DCQCN (`0xBFFE`) and
  SMaRTT (`0xBFFB`), with no shared algorithm code (disjoint `device/rp/*` trees).
  Trace format IDs 15-19 are Swift's.
- The default app in `host/pcc.c` is NVIDIA's `rtt_template`; Swift is selected at
  runtime via `--rp-swift` / `-rp-sw`.
- One ACTIVE PCC process per BlueField (firmware HA): you cannot run Swift and another
  PCC algorithm actively on the same card simultaneously.

### Where the credit-window variant went

A second variant, Swift-CX8, actuated this same cwnd through the
`results->credits` / `reload_credits` fields as a true credit window instead of
converting it to a rate. It now lives on its own branch, `swift-cx8` (a snapshot taken
at the point of removal), and is no longer part of this tree. On that branch it uses
algo_id `0xBFFA`, which this tree has since given to the `--rp-multi` mux; renumber it
before merging the two. The
reason is scope: the credit fields are dead on every device this DOCA release supports
(see "Actuating a window on a rate framework" above), so the variant compiles and loads
but runs as an uncontrolled sender. It can never be benchmarked on this hardware, and
the thesis does not cover it.

Keeping a non-runnable twin of Swift in the mainline had a real cost: every contract
change, comment pass, and README edit had to be mirrored into a second nearly identical
tree, and its presence next to the measurable Swift invited confusion about which
implementation the measurements refer to. The `swift-cx8` branch preserves the variant
in full (device tree, README, build and host glue, the `--rp-swift-cx8` flag) in case
future firmware backs the credit path.
