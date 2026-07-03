# DCQCN — ECN-based congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Zhu et al., "Congestion Control for Large-Scale RDMA Deployments", SIGCOMM 2015** —
specifically Equations (1)–(4) and the Figure 7 state machine. DCQCN is
rate-based and driven by **ECN / CNPs** (Congestion Notification Packets); unlike
[TIMELY](../timely/README.md) it does **not** use RTT as a signal.

## File layout

| File | Purpose |
|---|---|
| `algo/dcqcn.h` | Public API: `dcqcn_init`, `dcqcn_algo`, `dcqcn_set_algo_params` |
| `algo/dcqcn_ctxt.h` | Per-flow state (48 B): RC/RT rates, α, the three software timers/counters |
| `algo/dcqcn_algo_params.h` | Compiled-in default `#define`s (timer period, byte threshold, g, F, R_AI, R_HAI, rates) |
| `algo/dcqcn.c` | Algorithm: `dcqcn_init` (param/counter registration) + the Figure-7 handlers |
| `rp_dcqcn_dev_main.c` | DPA entry points (`doca_pcc_dev_user_algo`, `doca_pcc_dev_user_init`, `doca_pcc_dev_user_set_algo_params`) |

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

Device traces are **off by default** (per-event tracing is hot-path cost on every
TX). Build with `DCQCN_DEBUG=1` to compile in the format 6/7/8/9/10 device traces:

```bash
DCQCN_DEBUG=1 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Reading the trace output

The `doca_pcc` host process decodes device traces through the registered
`trace_message_formats[]` table in [`host/pcc_core.c`](../../../host/pcc_core.c),
so the stream is already human-readable, e.g.:

```
format 6 - pcc event: ev_type=5 flow_tag=188005017 port=1 algo_slot=15 rate=524288
format 8 - dcqcn cnp: old_rate=1048576 new_rate=524288 alpha=65536 dec_factor=32768 ts=...
```

All five DCQCN trace formats only emit when built `DCQCN_DEBUG=1`.

## Tuning parameters for your fabric

The 8 runtime-tunable parameters, their compiled-in defaults
(`algo/dcqcn_algo_params.h`), and the min/max enforced by the framework at
registration (`dcqcn_init`):

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

Rate parameters are **FXP20** fractions of line rate (`1<<20` = 100%), so their
absolute meaning scales with the NIC line rate — re-tune if the NIC speed changes.

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

Read all 8 back with a bulk get (`cmd_type=0xA`); the values land in `text[0]`
(`timer_period`) … `text[7]` (`min_rate`), in the index order above:

```bash
sudo mlxreg -d $MST -y --reg_name PPCC \
    --indexes "lp_msb=0,pnat=0,local_port=1,target_app=0,algo_slot=0,algo_param_index=0" \
    --op "cmd_type=0xA"
```

The 3 counters are scraped with `cmd_type=0xC` (read) / `0xD` (read-and-clear):
`text[0]` = TX events, `text[1]` = CNP events, `text[2]` = NACK events.

> **Note:** unlike TIMELY, this branch has no `dcqcn_params_dump` binary, so the
> `incast-bench` orchestrator has no auto-discovered contract for DCQCN — tune via
> raw `mlxreg` (indices above) until a dumper is added.

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

Registered in `host/pcc_core.c`; all DCQCN formats emit only when built
`DCQCN_DEBUG=1`.

| ID | Source | Fields |
|---|---|---|
| 6 | per-event summary (`rp_dcqcn_dev_main.c`) | `ev_type`; `flow_tag`; `port`; `algo_slot` (15 = ECE fallback, 0 = registered slot); `rate` (FXP20) |
| 7 | `dcqcn_handle_new_flow` | `rate` (FXP20 initial); `alpha` (FXP16, initial = 65536 = 1.0); `ts` (ns) |
| 8 | `dcqcn_handle_cnp` | `old_rate`; `new_rate`; `alpha`; `dec_factor` (FXP16 `1 − α/2`); `ts` |
| 9 | `dcqcn_handle_tx` | `cur_rate`; `alpha`; `t_count`; `bc_count`; `bytes_accum` |
| 10 | `dcqcn_rate_increase` | `old_rate`; `new_rate`; `target` (RT); `phase` (0 = FR, 1 = HI, 2 = AI); `t_bc` (`t_count<<16 \| bc_count`) |

### Events DCQCN acts on

`doca_pcc_dev_user_init` **masks ACK events** (`disable_event_bitmask =
DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK`) — DCQCN does not use per-ACK feedback.

| ev_type | Name | DCQCN action |
|---|---|---|
| 2 | `CNP` | cut rate: `RC ← RC·(1 − α/2)`, update α, reset timers/counters (Eq. 1) |
| 3 | `TX` | advance the α-decay / rate-increase timers and the byte counter; increase rate per Fig. 7 |
| 5 | `NACK` | conservative 50% cut + reset recovery state (paper-undefined) |
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
    Additive Incr.   min(T,BC) > F   :    RT ← RT + R_AI ; RC ← (RT+RC)/2 (Eq. 4)
    Hyper Increase   otherwise       :    RT ← RT + R_HAI; RC ← (RT+RC)/2
```

α is FXP16, rates are FXP20; `dcqcn_diff_with_wrap32` handles 32-bit timestamp
wraparound. Idle catch-up on the timers is bounded (`DCQCN_MAX_ALPHA_CATCHUP`,
`DCQCN_MAX_TIMER_CATCHUP`) to avoid unbounded loops / rate jumps after idle.

## Paper-silent design choices

The paper leaves several points undefined; the decisions made here are:

- **NACK handling**: paper assumes a PFC fabric where NACKs are rare — we apply a
  conservative 50% cut and reset the recovery state.
- **Hardware timers**: the paper's asynchronous NIC timers are advanced on TX
  events in DOCA's event-driven model (equivalent for active flows), with bounded
  catch-up after idle gaps.
- **Long-idle α**: clamped to 0 past `DCQCN_MAX_ALPHA_CATCHUP` elapsed periods
  (32 decays at g=1/256 would only reach ≈0.88·α anyway).
- **Byte-counter units**: paper defines B in bytes; the TX event exposes a
  cumulative 32-byte-unit counter (16-bit wrapping), tracked as a wrapping delta.

## Hardware / branch context

- Target: BlueField-3 (DPA), 100 Gbps RoCEv2. DCQCN requires **ECN marking at the
  switch** and CNP generation at the receiver, both configured on this fabric.
- Independent sibling of the [`timely`](../timely/README.md) branch: no shared
  algorithm code, but the host/build glue is structured so both can coexist
  (format IDs 7–10 are DCQCN's, 11–14 are TIMELY's).
- On a PFC + ECN fabric the NIC's own DCQCN may co-fire; when reading counters,
  remember measured throughput is the joint result of this RP logic and any
  hardware reaction, not this code alone.
