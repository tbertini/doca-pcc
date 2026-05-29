# TIMELY — RTT-based congestion control for DOCA PCC

Implementation of the RP (Reaction Point) algorithm from
**Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter", SIGCOMM 2015** —
specifically Algorithm 1 in §4.3 with the default parameters from §6.

## File layout

| File | Purpose |
|---|---|
| `algo/timely.h` | Public API: `timely_init`, `timely_algo`, `timely_set_algo_params` |
| `algo/timely_ctxt.h` | Per-flow state struct (cur_rate, prev_rtt, signed rtt_diff, HAI counter, RTT-probe abort machinery) |
| `algo/timely_algo_params.h` | Default parameters (T_low, T_high, δ, β, α, minRTT, HAI threshold, min rate) — every value either cites a paper section or is marked `FIXME:` if the paper is silent |
| `algo/timely.c` | Algorithm 1 implementation (see [§Algorithm 1 mapping](#algorithm-1-mapping)) |
| `rp_timely_dev_main.c` | DPA entry points (`doca_pcc_dev_user_algo`, `doca_pcc_dev_user_init`); slot-15 fallback bypass |
| `timely-prettify.sh` | Trace post-processor — translates numeric ev_type/branch into names, with colors and filters |

## Build & run

Device-side code is compiled by `dpacc` during `meson setup`, **not** during `ninja`. After any change to `algo/*.c`, `rp_timely_dev_main.c`, or `build_device_code.sh` you must re-run `meson setup --wipe`.

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70
```

Tracing is debug-on by default. To build a quiet binary (no format 6/11/12/14 emissions):

```bash
TIMELY_DEBUG=0 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
```

## Reading the trace output

Pipe the live program through the prettifier:

```bash
/tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 \
    | bash device/rp/timely/timely-prettify.sh
```

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

The defaults in `algo/timely_algo_params.h` (`MIN_RTT_NS=25000`, `T_LOW=50000`,
`T_HIGH=500000`, `DELTA_FXP20=105`) come from **paper §6.1, tuned for a 10 Gbps
fabric with ~20 µs base RTT**. On a different fabric you must retune. To get
recommended values for your specific link:

```bash
bash device/rp/timely/timely-recommend-params.sh --receiver <HOST> --iface mlx5_0
```

The script measures base RTT (via `ping`, taking the minimum to approximate
wire propagation delay) and reads the local NIC line rate, then prints the
four `#define` lines you should paste into `algo/timely_algo_params.h`. See
`--help` for flags. Re-run it any time the fabric or NIC speed changes.

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
new_rtt_diff  ← new_rtt - prev_rtt              # timely.c:443
prev_rtt      ← new_rtt                         # timely.c:444
rtt_diff      ← (1-α)·rtt_diff + α·new_rtt_diff # timely.c:451-455 (signed FXP16 EWMA)
normalized_gradient ← rtt_diff / minRTT         # timely.c:459

if new_rtt < T_low: rate ← rate + δ; return     # timely.c:466-474   (TIMELY_BRANCH_T_LOW)
if new_rtt > T_high:                            # timely.c:481-495   (TIMELY_BRANCH_T_HIGH)
    rate ← rate · (1 - β·(1 - T_high/new_rtt))
    return
if normalized_gradient ≤ 0:                     # timely.c:498-516   (TIMELY_BRANCH_GRAD_LE)
    N = 5 if neg_streak ≥ 5 else 1              # HAI; paper §4.3
    rate ← rate + N·δ
else:                                           # timely.c:518-540   (TIMELY_BRANCH_GRAD_GT)
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
- **minRTT:** paper says "fixed value… known ahead of time" without specifying. Default 25 µs.
- **min_rate:** paper does not define a floor. Default `1<<6` (FXP20), matches DCQCN.
- **NACK handling:** paper assumes PFC fabric where NACKs shouldn't happen. We do a conservative 50% cut.
- **CNP:** explicitly ignored per paper §1.
- **Rate update scaling per minRTT (§5):** paper mentions but doesn't formalize. We do naive per-event update.
- **δ (10 Mbps):** paper specifies as absolute bandwidth; we encode as FXP20 line-rate fraction (105 ≈ 10 Mbps on a 100 Gbps NIC). Re-tune if NIC speed changes.

## Hardware / branch context

- Tested on BlueField-3 (DPA) at `alveo-u50d-01`.
- Independent sibling of the `dcqcn` branch — no shared algorithm code, but the build/host glue is structured so both can coexist when merged (format IDs 7–10 are reserved for DCQCN's debug traces, 11–14 for TIMELY's).
- The default app in `host/pcc.c` is **unchanged from main** — TIMELY is selected at runtime via `--rp-timely` / `-rp-tm`.
