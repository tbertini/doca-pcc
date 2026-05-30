# TIMELY — RTT-based congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter", SIGCOMM 2015** —
specifically Algorithm 1 in §4.3 with the default parameters from §6.

## File layout

| File | Purpose |
|---|---|
| `algo/timely.h` | Public API: `timely_init`, `timely_algo`, `timely_set_algo_params` |
| `algo/timely_ctxt.h` | Per-flow state struct (cur_rate, prev_rtt, signed rtt_diff, HAI counter, RTT-probe abort machinery) |
| `algo/timely_algo_params.h` | Compiled-in default `#define`s (T_low, T_high, δ, β, α, minRTT, HAI threshold, min rate) — every value either cites a paper section or is marked `FIXME:` if the paper is silent |
| `algo/timely_params.h` | The PPCC **contract**: 10-param (9 tuning + 1 diagnostic `trace_on`) / 8-counter table (name, unit, default, min, max), shared between the DPA algorithm and the host-side `timely_params_dump`. Single source of truth for runtime overrides |
| `algo/timely.c` | Algorithm 1 implementation (see [§Algorithm 1 mapping](#algorithm-1-mapping)) |
| `rp_timely_dev_main.c` | DPA entry points (`doca_pcc_dev_user_algo`, `doca_pcc_dev_user_init`); slot-15 fallback bypass (see [§Slot 0 vs slot 15](#slot-0-vs-slot-15)) |

Host-side helper scripts (trace prettifier, mlxreg decoder) live in
[`tools/`](../../../tools) at the application root — they are not DPA code.

## Build & run

Device-side code is compiled by `dpacc` during `meson setup`, **not** during `ninja`. After any change to `algo/*.c`, `rp_timely_dev_main.c`, or `build_device_code.sh` you must re-run `meson setup --wipe`.

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70
```

Per-event traces (format 6/11/12/14) are **compiled in but quiet by default**: the DPA
emits nothing until asked. Turn them on at runtime — no rebuild — by setting the `trace_on`
PPCC param to 1 (and back to 0 to silence), as described in *Runtime override via PPCC* below.

To instead force traces on from boot — e.g. to catch the earliest events, before a param
write can land — build with `TIMELY_DEBUG=1`, which emits unconditionally and ignores the
runtime param:

```bash
TIMELY_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Reading the trace output

Pipe the live program through the prettifier:

```bash
/tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 \
    | bash tools/timely-prettify.sh
```

For decoding raw `mlxreg` PPCC register reads (param/counter dumps) into
labelled fields, see [`tools/mlxreg-pretty.py`](../../../tools/mlxreg-pretty.py).

Useful flags:

- `--filter-flow 1414831099` — keep only format-6 lines for one flow tag
- `--drop-acks` — drop ACK noise (most common format-6 line)
- `--drop-reserved` — drop the `format 7-10 reserved` placeholder lines
- `--no-color` — disable ANSI colors

Before / after example:

```
# raw
format 6 - pcc event: ev_type=4 flow_tag=1414831099 port=0 algo_slot=15 rate=64
format 12 - timely rtt: new_rtt=600000 rtt_diff=2484 norm_grad=0 branch=0x82 new_rate=64

# prettified
format 6 - pcc event: ev_type=ACK flow_tag=1414831099 port=0 algo_slot=15 rate=64
format 12 - timely rtt: new_rtt=600000 rtt_diff=2484 norm_grad=0 branch=GRAD_LE+CLAMP new_rate=64
```

## Tuning parameters for your fabric

The compiled-in defaults in `algo/timely_algo_params.h` are
`MIN_RTT_NS=3800`, `T_LOW_NS=10000`, `T_HIGH_NS=80000`, `DELTA_FXP20=1049`,
`BETA_FXP16=52429`, `ALPHA_FXP16=57344`. These were retuned for this cluster's
**100 Gbps BlueField-3 fabric with ~3.8 µs 1:1 base RTT** — they are *not* the
paper's 10 Gbps / ~20 µs §6.1 values. On a different fabric you must retune:
measure base RTT (e.g. minimum `ib_write_lat` or `ping`, approximating wire
propagation delay), and scale δ with the NIC line rate (δ/line_rate is the
dimensionless additive step). The two header values (`timely_algo_params.h`
`#define`s and the `timely_params.h` contract defaults) are kept in sync — edit
both, or rely on the PPCC runtime path below to override without recompiling.

### Runtime override via PPCC (no recompile)

The 9 parameters in `algo/timely_params.h` are also exposed through the
standard PPCC register interface, so a host-side orchestrator can push a new
vector between runs without rebuilding the DPA binary. The Mac-side
[`incast-bench`](https://github.com/tbertini/incast-bench) orchestrator uses this path:
it dumps the contract via `timely_params_dump` at start-of-run, pushes the
chosen vector via `mlxreg` `cmd_type=0xB` (bulk set params), and scrapes the
8 TIMELY counters via `cmd_type=0xC` / `0xD`. See `incast-bench --help` and
the `--timely-param` / `--timely-params` flags. Re-tuning the compiled-in
defaults is still worthwhile (faster startup, sane values when nothing pushes
overrides), but for sweeps the PPCC path is what you want.

The same channel carries the diagnostic `trace_on` toggle (param index 9): push
`trace_on=1` to start the format 6/11/12/14 trace stream and `trace_on=0` to stop it,
without restarting the binary. It defaults to 0 and is **not** an algorithm tuning knob,
so parameter sweeps should leave it alone. (Building with `TIMELY_DEBUG=1` forces tracing
on regardless of this param — see [Build & run](#build--run).)

To flip it on a *running* `doca_pcc` (default build only — `TIMELY_DEBUG=1` ignores it),
use `mlxreg` `cmd_type=0x8` (`set_param`) on `algo_slot=0`, `algo_param_index=9`. Note that
for writes `cmd_type` goes **inside `--set`** (no `--op`, no `--get`), and `value` holds the
new value:

```bash
sudo mst status                       # find your device, e.g. /dev/mst/mt41692_pciconf0
MST=/dev/mst/mt41692_pciconf0

# traces ON  (trace_on = 1)
sudo mlxreg -d $MST -y --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=9,target_app=0" \
    --set "cmd_type=0x8,value=0x1"

# traces OFF (trace_on = 0)
sudo mlxreg -d $MST -y --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=9,target_app=0" \
    --set "cmd_type=0x8,value=0x0"
```

`algo_slot=0` because the dispatcher always reads slot-0 params (see [Slot 0 vs slot 15](#slot-0-vs-slot-15)).
To read the value back, bulk-get all params (`cmd_type=0xA`) and inspect `text[9]`.

## Slot 0 vs slot 15

You will see two `algo_slot` values in the traces: **0** and **15**.

- **Slot 0** is where `doca_pcc_dev_user_init` registers TIMELY (`algo_idx=0`).
  Its periodic `rate=64 flow_tag=0` lines are framework heartbeat, not real
  traffic.
- **Slot 15** is the NIC's default fallback slot for QPs that did not negotiate
  a specific algorithm via ECE. Your `ib_write_bw` flows land here, so the real
  `format 12 - timely rtt` events carry `algo_slot=15`.

Both run TIMELY because `doca_pcc_dev_user_algo` **ignores the negotiated slot
and always reads slot 0's params/counters** (`rp_timely_dev_main.c`):

```c
uint32_t *param   = doca_pcc_dev_get_algo_params(port_num, 0);
uint32_t *counter = doca_pcc_dev_get_counters(port_num, 0);
```

This is deliberate: TIMELY is the only algorithm in the binary, so slot
negotiation is irrelevant. The practical consequence for tuning: **a parameter
vector pushed via PPCC `algo_slot=0` governs the slot-15 benchmark traffic**,
and the 8 TIMELY counters read from slot 0 aggregate that traffic. If a future
build runs several algorithms side-by-side, this dispatcher must instead read
`attr->algo_slot` and map it to the matching algorithm.

## Trace format reference

### Registered formats (`host/pcc_core.c`)

| ID | Source | Field meanings |
|---|---|---|
| 0 | `user init` (every algo's init) | `port_num`; `algo_index` (0 for TIMELY); `algo_slot` (0..15); `algo_enable`; `disable_event_bitmask` (0x0 — TIMELY masks nothing) |
| 1 | DOCA infra `user algo` | `algo_slot`; `result_rate` (FXP20); `result_rtt_req`; `port_num`; `timestamp` |
| 2–5 | NP-side `pcc_np dev` | Only fire when running a `--np-*` app — ignore on RP/TIMELY |
| 6 | per-event summary (`rp_timely_dev_main.c`) | `ev_type` (see table below); `flow_tag`; `port`; `algo_slot` (15 = ECE fallback / no negotiation, 0 = registered TIMELY slot); `rate` (FXP20 after our algo ran) |
| 7–10 | reserved for DCQCN | placeholder strings — never emitted on the timely branch |
| 11 | TIMELY `new_flow` | `rate` (FXP20 initial = 1048576 = full line rate); `ts` (ns) |
| 12 | TIMELY `rtt` (Algorithm 1 ran) | `new_rtt` (ns); `rtt_diff` (signed EWMA, ns); `norm_grad` (signed `rtt_diff/min_rtt`); `branch` (see table below); `new_rate` (FXP20) |
| 13 | retired | clamp info folded into format-12 `branch` bit `0x80` |
| 14 | TIMELY `hai` | `neg_count` (consecutive ≤0 gradients); `N` (HAI multiplier — 1 normally, 5 in HAI mode); `new_rate`; `delta` (additive step δ) |

### `ev_type` (from [`include/doca_pcc_dev.h:46-52`](../../../../../include/doca_pcc_dev.h))

| ev_type | Name | Meaning |
|---|---|---|
| 0 | `NULL` | Unspecified / no event |
| 1 | `FW` | Deprecated, not used |
| 2 | `CNP` | RoCE Congestion Notification Packet received |
| 3 | `TX` | TX packet burst transition ended |
| 4 | `ACK` | RoCE ACK received |
| 5 | `NACK` | RoCE NACK received |
| 6 | `RTT` | RTT probe response received — TIMELY's main signal |

TIMELY adjusts the rate only on `ev_type=6` (RTT). TX events maintain the RTT-probe abort state machine (re-issue dropped probes). NACK does a 50% rate cut + reset HAI counter (paper-undefined, conservative). CNP is ignored by design (paper §1: TIMELY does not use ECN/CNP).

### `branch` (format 12, low nibble = Algorithm-1 branch; bit `0x80` = clamped)

| branch | Algorithm 1 line | Action |
|---|---|---|
| `0x00` `T_LOW` | `if new_rtt < T_low` | `rate ← rate + δ` (additive) |
| `0x01` `T_HIGH` | `if new_rtt > T_high` | `rate ← rate · (1 - β·(1 - T_high/new_rtt))` (multiplicative) |
| `0x02` `GRAD_LE` | `if normalized_gradient ≤ 0` | `rate ← rate + N·δ` (HAI mode N=5 if streak ≥ 5) |
| `0x03` `GRAD_GT` | `else` | `rate ← rate · (1 - β·normalized_gradient)` (gradient-scaled multiplicative) |
| `0x80` bit | — | rate was clamped to MIN_RATE or DOCA_PCC_DEV_MAX_RATE this event |

Combined values: `0x82` = GRAD_LE branch + clamped, `0x81` = T_HIGH branch + clamped, etc.

## Algorithm 1 mapping

Pseudocode from paper §4.3, with code line refs:

```
Input: new_rtt
new_rtt_diff  ← new_rtt - prev_rtt
prev_rtt      ← new_rtt
rtt_diff      ← (1-α)·rtt_diff + α·new_rtt_diff   (signed FXP16 EWMA)
normalized_gradient ← rtt_diff / minRTT

if new_rtt < T_low: rate ← rate + δ; return       (TIMELY_BRANCH_T_LOW)
if new_rtt > T_high:                              (TIMELY_BRANCH_T_HIGH)
    rate ← rate · (1 - β·(1 - T_high/new_rtt))
    return
if normalized_gradient ≤ 0:                       (TIMELY_BRANCH_GRAD_LE)
    N = 5 if neg_streak ≥ 5 else 1                (HAI; paper §4.3)
    rate ← rate + N·δ
else:                                             (TIMELY_BRANCH_GRAD_GT)
    rate ← rate · (1 - β·normalized_gradient)
```

Default parameters, units, and min/max bounds are in [`algo/timely_params.h`](algo/timely_params.h) — the single source of truth for the PPCC contract, shared between the DPA algorithm and the host-side dumper. Per-event params are runtime-configurable via the standard DOCA PCC param interface (9 params, see `TIMELY_P_*` enum in [`algo/timely_params.h`](algo/timely_params.h); 8 counters via `TIMELY_C_*`).

## Known unknowns / FIXMEs

The paper is silent on several points; every choice is flagged in the source so they can be revisited:

```bash
grep -rn "FIXME:" device/rp/timely/algo/ device/rp/timely/rp_timely_dev_main.c
```

Highlights:
- **α (EWMA weight):** paper labels it but never gives a value. Default 0.875 (FXP16 = 57344).
- **minRTT:** paper says "fixed value… known ahead of time" without specifying. Default 3.8 µs (`3800` ns — measured BF3-to-BF3 1:1 `ib_write_bw`).
- **min_rate:** paper does not define a floor. Default `1<<6` (FXP20), matches DCQCN.
- **NACK handling:** paper assumes PFC fabric where NACKs shouldn't happen. We do a conservative 50% cut.
- **CNP:** explicitly ignored per paper §1.
- **Rate update scaling per minRTT (§5):** paper mentions but doesn't formalize. We do naive per-event update.
- **δ:** paper specifies as absolute bandwidth (10 Mbps); we encode as an FXP20 line-rate fraction. Default `1049` ≈ 100 Mbps on a 100 Gbps NIC (retuned 2026-05-03; the paper's 10 Mbps would be ~`105`). Re-tune if NIC speed changes.

## Hardware / branch context

- Tested on BlueField-3 (DPA) at `alveo-u50d-01`.
- Independent sibling of the `dcqcn` branch — no shared algorithm code, but the build/host glue is structured so both can coexist when merged (format IDs 7–10 are reserved for DCQCN's debug traces, 11–14 for TIMELY's).
- The default app in `host/pcc.c` is **unchanged from main** — TIMELY is selected at runtime via `--rp-timely` / `-rp-tm`.
- On a PFC + ECN fabric the NIC's DCQCN co-fires alongside TIMELY: you will see
  receiver-side CNPs even when TIMELY is driving the rate. When reading the
  TIMELY counters, remember that measured throughput is the joint result of
  TIMELY's RTT-based rate control **and** DCQCN's ECN reaction, not TIMELY alone.
