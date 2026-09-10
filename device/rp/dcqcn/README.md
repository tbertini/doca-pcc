# DCQCN — ECN-based congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Zhu et al., "Congestion Control for Large-Scale RDMA Deployments", SIGCOMM 2015** —
specifically Equations (1)–(4) and the Figure 7 state machine. DCQCN is
rate-based and driven by **ECN / CNPs** (Congestion Notification Packets); unlike
[TIMELY](../timely/README.md) it does **not** use RTT as a signal.

## File layout

| File | Purpose |
|---|---|
| `algo/dcqcn.h` | Public API (`dcqcn_init`, `dcqcn_algo`, `dcqcn_set_algo_params`) + the runtime trace gate (`DCQCN_TRACE_ENABLED`) and `DCQCN_TRACE_TSFLOW` packing macro shared with `rp_dcqcn_dev_main.c` |
| `algo/dcqcn_ctxt.h` | Per-flow state (48 B): RC/RT rates, α, the three software timers/counters |
| `algo/dcqcn_algo_params.h` | Compiled-in default `#define`s (timer period, byte threshold, g, F, R_AI, R_HAI, rates) |
| `algo/dcqcn_params.h` | **PPCC contract** (single source of truth): param/counter indices + `{name, unit, def, min, max}` table, included by both `dcqcn.c` and the host dumper |
| `algo/dcqcn.c` | Algorithm: `dcqcn_init` (param/counter registration from `dcqcn_params.h`) + the Figure-7 handlers |
| `rp_dcqcn_dev_main.c` | DPA entry points (`doca_pcc_dev_user_algo`, `doca_pcc_dev_user_init`, `doca_pcc_dev_user_set_algo_params`) |
| `../../../host/dcqcn_params_dump.c` | Host-side, no-DOCA-deps binary that emits the `dcqcn_params.h` contract as JSON for `incast-bench` |

The host glue (trace format strings, arg parsing) lives in
[`host/pcc_core.c`](../../../host/pcc_core.c); DCQCN's `algo_id` is **`0xBFFE`**.

## Build & run

Device-side code is compiled by `dpacc` during `meson setup`, **not** during
`ninja`. After any change under `device/rp/dcqcn/` you must re-run
`meson setup --wipe`.

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc -d mlx5_0 -l 70          # DCQCN is the default app on this branch
```

On this branch the default application is DCQCN (`host/pcc.c` sets
`cfg.app = pcc_rp_dcqcn_app`), so a bare `doca_pcc -d mlx5_0` runs it; the
`--rp-dcqcn` / `-rp-dc` flag selects it explicitly.

Per-event traces (formats 6-10) are **compiled in but quiet by default** (per-event
tracing would otherwise be hot-path cost on every TX). Turn them on at runtime — no
rebuild — by setting the `trace_on` PPCC param (index 8) to 1, back to 0 to silence.
To force them on from boot (e.g. to catch the earliest events, before a param write
can land), build with `DCQCN_DEBUG=1` instead, which ignores the param:

```bash
DCQCN_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Reading the trace output

The `doca_pcc` host process decodes device traces through the registered
`trace_message_formats[]` table in [`host/pcc_core.c`](../../../host/pcc_core.c),
so the stream is already human-readable, e.g.:

```
format 6 - pcc event: tsflow=0xa2f3c1000b322559 ev_type=5 port=1 algo_slot=15 rate=524288
format 8 - dcqcn cnp: tsflow=0xa2f3c1000b322559 old_rate=1048576 alpha=65536 dec_factor=32768 new_rate=524288
```

`tsflow` packs the triggering event's timestamp and flow_tag into one 64-bit value —
see [Trace format reference](#trace-format-reference) for the bit layout. All DCQCN
trace formats are gated by the runtime `trace_on` param (or always on if built
`DCQCN_DEBUG=1`); see [Runtime tuning via PPCC](#runtime-tuning-via-ppcc-incast-bench-integration).

## Tuning parameters for your fabric

`algo/dcqcn_params.h` is the single source of truth for the 9 PPCC-exposed
parameters (8 tuning + 1 diagnostic `trace_on`), their defaults, and the min/max
enforced by the framework at registration (`dcqcn_init`) — it replaces the enum +
inline `doca_pcc_dev_algo_init_param` argument lists that used to live directly in
`dcqcn.c`. The table below is generated from that contract:

| idx | name | unit | default | min | max | meaning |
|---|---|---|---|---|---|---|
| 0 | `timer_period` | ns | 55000 | 1 | UINT32_MAX | rate-increase / α-decay timer period T |
| 1 | `byte_counter_th` | bytes | 10485760 | 1 | UINT32_MAX | byte-counter threshold B (10 MiB) |
| 2 | `g_shift` | shift | 8 | 1 | **16** | α EWMA weight, g = 1/(1<<g_shift) |
| 3 | `f_threshold` | count | 5 | 1 | 100 | fast-recovery step threshold F |
| 4 | `r_ai` | FXP20 | 419 | 1 | 1048576 | additive-increase step R_AI |
| 5 | `r_hai` | FXP20 | 2095 | 1 | 1048576 | hyper-increase step R_HAI (= 5·R_AI) |
| 6 | `new_flow_rate` | FXP20 | 1048576 | 1 | 1048576 | initial rate for new flows (100% line) |
| 7 | `min_rate` | FXP20 | 64 | 1 | 1048576 | minimum rate floor |
| 8 | `trace_on` | bool | 0 | **0** | 1 | runtime device-trace toggle — diagnostics only, not a tuning knob (see [Runtime tuning via PPCC](#runtime-tuning-via-ppcc-incast-bench-integration)) |

Rate parameters are **FXP20** fractions of line rate (`1<<20` = 100%), so their
absolute meaning scales with the NIC line rate — re-tune if the NIC speed changes.
`trace_on` is the odd one out in this table: unlike 0-7 (all registered with a
framework MIN of 1, transcribed verbatim from the pre-contract code), its min is 0
on purpose, so it can be switched back off at runtime — see `dcqcn_params.h`.

> **`g_shift` is capped at 16 on purpose.** `dcqcn_alpha_update_cnp` computes
> `1 << (16 - g_shift)`; a value above 16 would shift by a negative count
> (undefined behavior). The framework rejects any push of `g_shift > 16`.

### Runtime override via PPCC (no recompile)

The parameters are exposed through the standard PPCC `mlxreg` register, so they
can be changed on a running `doca_pcc` without rebuilding. Set a single value with
`cmd_type=0x8` (write goes **inside `--set`**, no `--op`); e.g. set `g_shift`
(index 2) to 10:

```bash
sudo mst status                        # find the device, e.g. /dev/mst/mt41692_pciconf0
MST=/dev/mst/mt41692_pciconf0
sudo mlxreg -d $MST -y --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=2,target_app=0" \
    --set "cmd_type=0x8,value=0xA"
```

Read all 9 back with a bulk get (`cmd_type=0xA`); the values land in `text[0]`
(`timer_period`) … `text[8]` (`trace_on`), in the index order above:

```bash
sudo mlxreg -d $MST -y --reg_name PPCC \
    --indexes "lp_msb=0,pnat=0,local_port=1,target_app=0,algo_slot=0,algo_param_index=0" \
    --op "cmd_type=0xA"
```

The 3 counters are scraped with `cmd_type=0xC` (read) / `0xD` (read-and-clear):
`text[0]` = TX events, `text[1]` = CNP events, `text[2]` = NACK events.

## Runtime tuning via PPCC (incast-bench integration)

Like TIMELY and Swift, DCQCN's parameter/counter contract lives in a single header
(`algo/dcqcn_params.h`) included by both the DPA algorithm (`dcqcn.c`) and a small
host-side dumper (`host/dcqcn_params_dump.c`), so the orchestrator's view of the
contract can never drift from what the device actually registers.

Build the dumper alongside the app (meson adds it as its own executable target,
same pattern as `swift_params_dump`):

```bash
ninja -C /tmp/build dcqcn_params_dump
/tmp/build/pcc/dcqcn_params_dump | python3 -m json.tool
```

It prints `algo_id` (`0xBFFE` = 49150), one `{idx, name, unit, default, min, max}`
entry per parameter (the 9 rows in the table above, keyed by the same `name`s), and
one `{idx, name}` entry per counter — no DOCA dependency, so it also builds and runs
on a plain Mac/Linux host for offline inspection.

`incast-bench` dumps this contract at session start-of-run, then lets you override
any subset of parameters by their wire `name` with `--dcqcn-param KEY=VALUE`
(repeatable) or a full vector file via `--dcqcn-params FILE`, e.g.:

```bash
incast-bench ... --dcqcn-param g_shift=10 --dcqcn-param r_ai=838
```

Overrides are pushed as a single bulk `mlxreg cmd_type=0xB` vector; unspecified
params keep the compiled-in defaults from the table above. As with every DCQCN
param push, this must target `algo_slot=0`; it lands correctly only because the
device side pins its own lookup to **port 0** regardless of where RoCE traffic
actually arrives — see [Slot 0 / port 0 dispatch](#slot-0--port-0-dispatch) below
for why.

**Runtime `trace_on` toggle (like Swift's contract).** `trace_on` (index 8, kept
last) is diagnostics-only, not a tuning knob, but it rides the same PPCC channel as
the 8 tuning params above: push `algo_param_index=8,value=1` (bulk vector or a
single `cmd_type=0x8` write) to turn formats 6-10 on on a running binary, `value=0`
to silence — no rebuild, no `meson setup --wipe`. Sweep scripts should leave it at
its default (0) unless actively capturing a trace, since an enabled trace is extra
per-event work on the hot TX path. Building with `DCQCN_DEBUG=1` instead forces
every format on from boot regardless of this param (see [Build & run](#build--run)) —
useful for catching the very first events, before any param write can land.

## Slot 0 / port 0 dispatch

`doca_pcc_dev_user_algo` **ignores the negotiated `algo_slot`** and always reads
slot 0, and — critically — pins to **port 0**, not the event's port
(`rp_dcqcn_dev_main.c`):

```c
uint32_t *param   = doca_pcc_dev_get_algo_params(0, 0);
uint32_t *counter = doca_pcc_dev_get_counters(0, 0);
```

`dcqcn_init` registers DCQCN in slot 0 on every port, so slot negotiation is
irrelevant while DCQCN is the only algorithm in the binary. If a future build runs
several algorithms side by side, this dispatcher must instead read `attr->algo_slot`
and map it to the matching algorithm.

**Why port 0 (not the event port).** Param/counter arrays are per `(port, slot)`.
The orchestrator pushes params and scrapes counters over mlxreg PPCC `local_port=1`,
which firmware maps to **DPA `port_num=0`** (the `set_algo_params` callback fires
with `port_num=0`). But RoCE traffic on `mlx5_0` reaches the algo on **DPA
`port_num=1`** (format-6 events show `port=1`). Fetching for the *event* port would
read an array the push never touched — so a pushed value reads back fine via
`cmd_type=0xA` yet never reaches the running algorithm, and counter increments
strand on a port the scrape never reads. This was HW-confirmed on the
[timely](../timely/README.md) branch; the pin keeps the algo, the param push, and
the counter scrape on one array. Revisit if a build ever drives multiple physical
ports.

## Trace format reference

Registered in `host/pcc_core.c`; formats 6-10 are gated by the runtime `trace_on`
PPCC param (index 8, default 0), or always on if built `DCQCN_DEBUG=1` — see
[Build & run](#build--run) and
[Runtime tuning via PPCC](#runtime-tuning-via-ppcc-incast-bench-integration).

**`tsflow` (arg1 of every format below).** As of the 2026-07 trace-format
normalization, all five formats pack the triggering event's timestamp and flow_tag
into a single 64-bit `arg1`, printed as `tsflow=0x%lx`, via
`DCQCN_TRACE_TSFLOW(ts, flow)` (`dcqcn.h`):

```
bits [63:32]  timestamp, ns (doca_pcc_dev_get_timestamp) -- wraps every ~4.295 s
bits [31:0]   flow_tag      (doca_pcc_dev_get_flowtag)
```

This replaced five separate, inconsistent argument layouts with one convention:
`tsflow` first, then the format's own fields in a fixed order, `rate`/`new_rate`
(if present) always last. BEFORE → AFTER map (both columns are the 5 args of a
`doca_pcc_dev_trace_5` call, in order):

| ID | Source | BEFORE | AFTER |
|---|---|---|---|
| 6 | per-event summary (`rp_dcqcn_dev_main.c`) | `ev_type`; `flow_tag`; `port`; `algo_slot`; `rate` | `tsflow`; `ev_type`; `port`; `algo_slot`; `rate` |
| 7 | `dcqcn_handle_new_flow` | `rate`; `alpha`; `ts`; *0 (pad)*; *0 (pad)* | `tsflow`; `alpha`; `rate` |
| 8 | `dcqcn_handle_cnp` | `old_rate`; `new_rate`; `alpha`; `dec_factor`; `ts` | `tsflow`; `old_rate`; `alpha`; `dec_factor`; `new_rate` |
| 9 | `dcqcn_handle_tx` | `cur_rate`; `alpha`; `t_count`; `bc_count`; `bytes_accum` | `tsflow`; `alpha`; `t_bc`; `bytes_accum`; `cur_rate` |
| 10 | `dcqcn_rate_increase` | `old_rate`; `new_rate`; `target`; `phase`; `t_bc` | `tsflow`; `old_rate`; `target`; `phase`; `new_rate` |

Field notes (AFTER column), all fields printed `%lu` unless noted:

- **`rate` / `new_rate` / `cur_rate`** (FXP20) is always the last field when
  present (formats 7, 8, 9, 10) — this is the new, post-update rate in every case.
- **`alpha`** (FXP16, `1<<16` = 1.0): format 7's initial value (always `65536` for
  a new flow); format 8/9's current EWMA value at the time of the event.
- **`dec_factor`** (format 8, FXP16): the CNP multiplicative-decrease factor
  `1 − α/2` applied to `old_rate` to produce `new_rate`.
- **`target`** (format 10): RT, the recovery target rate (FXP20).
- **`phase`** (format 10): `0` = Fast Recovery, `1` = Hyper Increase, `2` =
  Additive Increase (paper Fig. 7) — encoding unchanged by this normalization.
- **`t_bc`** (format 9, `%lx`): `bits[31:16] = t_count`, `bits[15:0] = bc_count`
  (`(uint32_t)t_count << 16 | (bc_count & 0xFFFF)`).
- **format 10 dropped `t_bc`.** `dcqcn_rate_increase` fires up to
  `DCQCN_MAX_TIMER_CATCHUP` (8) times from the rate-increase timer's catch-up loop,
  plus up to `DCQCN_MAX_BYTE_CATCHUP` (8) more from the byte counter, all within a
  single TX event — so the
  same `(t_count, bc_count)` pair used to repeat on every format-10 line for that
  event. It's still visible once per TX event on the enclosing format-9 line, which
  shares the same `tsflow` (both fire from the same event), so a format-10 burst
  can be correlated back to its `t_count`/`bc_count` via that shared `tsflow`.
- Format 7's two trailing BEFORE args were always `0` (dead padding on a 5-slot
  `doca_pcc_dev_trace_5` call with only 3 real fields); the AFTER host format
  string just has 3 conversions instead of 5, not 3 real fields + 2 zeros.

### Runtime trace toggle (`trace_on`, param index 8)

Formats 6-10 are compiled in unconditionally and gated by
`DCQCN_TRACE_ENABLED(param)` (`dcqcn.h`), the same model as Swift's `trace_on`:

- **`-DDCQCN_DEBUG`** (compile time): `DCQCN_TRACE_ENABLED` is hardwired to `1` —
  every format fires from the very first event, and the `trace_on` param's value
  has no effect (it's still PPCC-readable/writable, just ignored by the gate).
- **default build** (no `-DDCQCN_DEBUG`): `DCQCN_TRACE_ENABLED(param)` reads
  `param[DCQCN_PARAM_TRACE_ON]` — traces are silent until something pushes
  `trace_on=1` over PPCC (`algo_param_index=8`), and silenced again by pushing
  `trace_on=0`. Either direction is a live param write — no rebuild, no
  `meson setup --wipe`.

Cost when disabled: one well-predicted branch per event; the trace-record write
and `doca_pcc_dev_trace_flush()` (the expensive part) are skipped entirely.

### Events DCQCN acts on

`doca_pcc_dev_user_init` **masks ACK events** (`disable_event_bitmask =
DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK`) — DCQCN does not use per-ACK feedback.

| ev_type | Name | DCQCN action |
|---|---|---|
| 2 | `CNP` | cut rate: `RC ← RC·(1 − α/2)`, update α, reset timers/counters (Eq. 1) |
| 3 | `TX` | advance the α-decay / rate-increase timers and the byte counter; increase rate per Fig. 7 |
| 5 | `NACK` | ignored (counted only) — paper has no NACK input; fabric is PFC-protected |
| 6 | `RTT` | ignored (pass-through) — DCQCN is not RTT-driven |

New flows are detected by `cur_rate == 0` (first event on a zeroed context) and
initialized to `new_flow_rate` with α = 1.0.

## Algorithm mapping (paper §3.1, Fig. 7)

```
On CNP (CutRate):                         RT ← RC
                                          RC ← RC · (1 − α/2)            (Eq. 1)
                                          α  ← (1−g)·α + g               (Eq. 1)
                                          reset Timer, ByteCounter, T, BC
On α-timer (every timer_period):          α  ← (1−g)·α                   (Eq. 2)
On rate-increase (Timer T or ByteCtr BC):
    Fast Recovery    max(T,BC) < F   :    RC ← (RT + RC)/2               (Eq. 3)
    Hyper Increase   min(T,BC) > F   :    RT ← RT + R_HAI; RC ← (RT+RC)/2
    Additive Incr.   otherwise       :    RT ← RT + R_AI ; RC ← (RT+RC)/2 (Eq. 4)
```

α is FXP16, rates are FXP20; `dcqcn_diff_with_wrap32` handles 32-bit timestamp
wraparound. Idle catch-up is bounded on all three (`DCQCN_MAX_ALPHA_CATCHUP` = 32,
`DCQCN_MAX_TIMER_CATCHUP` = 8, `DCQCN_MAX_BYTE_CATCHUP` = 8) to avoid unbounded loops
and rate jumps after idle.

**Both the rate-increase timer and the byte counter consume whole units and carry the
remainder.** The timer advances `last_rate_timer_ts += n_timer * timer_period` (not
`= timestamp`) and the byte counter subtracts `n_bc * byte_threshold` (not a single
`byte_threshold`), so neither runs slow just because the DPA only gets to observe them
on TX events. Both drop the backlog instead of carrying it once their catch-up bound is
hit, since carrying it would pin them at the bound on every subsequent event. At the
default `byte_counter_th` = 10 MiB a TX event never carries more than one multiple, so
the byte counter's loop always runs exactly once.

## Paper-silent design choices

The paper leaves several points undefined; the decisions made here are:

- **NACK handling**: the paper assumes a PFC fabric where congestion never drops
  packets, so its algorithm has no NACK input — matched exactly: NACK events are
  ignored (still counted via `DCQCN_COUNTER_NACK_EVENT` as the per-run proof that
  the PFC precondition held). The fabric baseline is applied per run by
  incast-bench (u55c ConnectX QoS via mstreg; A/B-validated 2026-08-07). While running
  this algorithm with PFC correctly configured, NACK events were never observed.
- **Hardware timers**: the paper's asynchronous NIC timers are advanced on TX
  events in DOCA's event-driven model (equivalent for active flows, and a flow with
  no TX events never increases its rate), with bounded catch-up after idle gaps.
- **Long-idle α**: clamped to 0 past `DCQCN_MAX_ALPHA_CATCHUP` elapsed periods. The
  decay loop's own fixed point is α < `1<<g_shift` (α ≈ 0 in FXP16), reached only
  after ~1417 iterations at g=1/256 — far too many for the hot path — so 0 is the
  best available approximation for a genuinely long gap. It is wrong in the band
  between the bound and that fixed point, where the true α is still ≈0.88·α at 32
  periods.
- **Byte-counter units**: paper defines B in bytes; the TX event exposes a
  cumulative 32-byte-unit counter (16-bit wrapping), tracked as a wrapping delta.
  This assumes the counter does not wrap twice between two TX events of a flow
  (2 MiB, ≈167 µs at 100 Gbps).
- **First event of a flow**: consumed by context initialization, so it is not also
  dispatched to a handler — a flow whose very first event is a CNP does not cut, and
  its first TX event does not increment `tx_events`. `prev_tx_cntr_32b` starts at 0
  rather than at the event's counter value, which offsets the first byte-counter
  interval by at most 2 MiB; the remainder carries, so this is a one-time phase
  offset and not a drift. The counter cannot be read during initialization because
  `doca_pcc_dev_get_roce_tx_cntrs` is documented "For TX events only".

## Hardware / branch context

- Target: BlueField-3 (DPA), 100 Gbps RoCEv2. DCQCN requires **ECN marking at the
  switch** and CNP generation at the receiver, both configured on this fabric.
- Independent sibling of the [`timely`](../timely/README.md) branch: no shared
  algorithm code, but the host/build glue is structured so both can coexist
  (format IDs 7–10 are DCQCN's, 11–14 are TIMELY's).
- On a PFC + ECN fabric the NIC's own DCQCN may co-fire; when reading counters,
  remember measured throughput is the joint result of this RP logic and any
  hardware reaction, not this code alone.
