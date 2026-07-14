# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

NVIDIA's DOCA PCC (Programmable Congestion Control) reference application for BlueField-3, forked and extended with custom RoCE congestion-control algorithms (TIMELY, DCQCN, Swift). A host-side Linux app (`host/`) loads DPA (Data Path Accelerator, RISC-V) kernels (`device/`) onto the NIC; the DPA code handles per-flow CC events (RTT probes, ACK/NACK, CNP, TX) and sets each flow's TX rate.

Each algorithm's `device/rp/<algo>/README.md` is the authoritative doc for that algorithm — read it before touching algorithm code. It covers the math, the fixed-point tricks, the trace formats, and the open FIXMEs.

## Environment and deployment — read first

- **Nothing in this checkout compiles on this Mac.** `dpacc`, the DOCA SDK, and the hardware live on the BlueField-3 host `alveo-u50d-01` under `/opt/mellanox/doca/applications`. Code flow: edit here → commit → push to `github.com/tbertini/doca-pcc` → pull and build on the BF3 side.
- This repo is the `pcc` subtree of a full DOCA 3.x tree. DOCA API headers for reference (e.g. `doca_pcc_dev*.h`, `doca_pcc_dev_data_structure_le_bf3.h`) are at `~/Desktop/doca/include/` — grep there to answer "does the DPA API expose X?".
- The Mac-side benchmark orchestrator (`incast-bench`) lives at `~/Desktop/incast-bench` and talks to the running algorithm via the PPCC register interface (see below).

## Build & run (on the BF3 host, not here)

```bash
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
/tmp/build/pcc/doca_pcc --rp-swift -d mlx5_0 -l 70    # -l 70 = show device traces
```

- **Device code is compiled by `dpacc` during `meson setup`, not during `ninja`.** After any change under `device/` or to `build_device_code.sh`, re-run `meson setup --wipe`; a bare `ninja` silently reuses the stale device binary. Host-only changes rebuild fine with plain `ninja`.
- `SWIFT_DEBUG=1 meson setup ...` force-enables Swift's per-event traces from boot; normally they are compiled in but gated by the runtime `trace_on` PPCC param (flip at runtime, no rebuild).
- There is no test suite or linter. Verification = run on hardware, read device traces (`-l 70`) and PPCC counters, compare against the algorithm's README trace-format tables.
- The algorithm is selected at runtime by CLI flag (`--rp-swift`/`-rp-sw`, `--rp-switch-telemetry`, `--np-nic-telemetry`, ...); no flag = the `rtt_template` app. `pcc_params.json` is the JSON-file equivalent of the CLI flags.
- One ACTIVE PCC process per BlueField (firmware HA): two algorithms cannot run on the same card simultaneously.

## Branch model

`main` is the untouched NVIDIA baseline. Each algorithm is an independent sibling branch — a disjoint `device/rp/<algo>/` tree plus additive host/build glue — designed to merge later into one runtime-selectable binary without conflicts:

| Branch | Algorithm | algo_id | Trace format IDs |
|---|---|---|---|
| `timely` | TIMELY (RTT gradient) | 0xBFFD | 11–14 (13 retired) |
| `dcqcn` | DCQCN (ECN/CNP) | 0xBFFE | 7–10 |
| `swift` | Swift (absolute delay, windowed) | 0xBFFC | 15–19 |
| all | templates (`rtt_template` = 0xBFFF) | — | 6 = shared per-event summary |

Trace-format IDs and algo_ids are a cross-branch registry — never reuse another branch's. Host-side helper scripts (`tools/mlxreg-pretty.py`, trace prettifiers) live on the `timely` branch. `timely-runtime-trace-toggle` was the structural template for the swift branch.

## Architecture

### Per-algorithm layout (`device/rp/<algo>/`)

- `rp_<algo>_dev_main.c` — DPA entry points: `doca_pcc_dev_user_init` (registers the algo on all ports/slots), `doca_pcc_dev_user_algo` (per-event dispatch), `doca_pcc_dev_user_set_algo_params` (PPCC writes).
- `algo/<algo>.c/.h` — the algorithm itself (init / event handler / param setter).
- `algo/<algo>_ctxt.h` — per-flow state struct.
- `algo/<algo>_algo_params.h` — compile-time defaults; every value that isn't paper-backed is marked `FIXME:` and is a seed to sweep.
- `algo/<algo>_params.h` — the PPCC contract (param/counter index enums + descriptor table with name/unit/default/min/max). **Single source of truth**, shared by the DPA code and the host-side `host/<algo>_params_dump.c` (a no-DOCA-deps binary that emits the contract as JSON for incast-bench). Param tables are **append-only** — orchestrators address params by index — and the `trace_on` diagnostic param stays last.

### Host side (`host/`)

`pcc.c` (main + config), `pcc_core.c` (argp flag registration, DOCA PCC setup, mailbox that sends port bandwidth to the device, and the **trace format strings** — indexed by format ID, they must match the device's `doca_pcc_dev_trace_5` calls). Adding an algorithm touches: `build_device_code.sh` (new `elif` block), `meson.build` (new dpacc stanza + dependency), `pcc_core.h` (extern app struct), `pcc_core.c` (flag + callback), optionally `host/<algo>_params_dump.c` (+ meson stanza).

### Runtime tuning path (PPCC)

The 19-ish tuning params per algorithm are exposed through the standard PPCC register interface: push a vector with `mlxreg ... cmd_type=0xB`, read counters with `0xC`/`0xD` — no recompile between sweep runs. incast-bench drives this using the `*_params_dump` JSON.

## BF3 / DPA hard constraints

- **Per-flow context is capped at 48 bytes** (`doca_pcc_dev_algo_ctxt_t`). Swift's ctxt is exactly 48 — adding state requires stealing bits.
- **No float, no sqrt, no libm** on the DPA. Fixed-point conventions: FXP16 for cwnd/gains/ratios (`1<<16` = 1.0), FXP20 for rate (`1<<20` = 100% line rate). Transcendentals via `__dpa_fxp_log2` / `__dpa_fxp_pow2` intrinsics (e.g. `1/√x = 2^(−½·log2 x)`).
- **`rate` is the only actuator.** `doca_pcc_dev_results_t.credits`/`reload_credits` are unbacked on BF3 — window-based algorithms must keep cwnd as the control variable and map it to a rate via the BDP identity (see Swift README).
- `doca_pcc_dev_get_ttl_hoplimit()` returns 0 on BF3 — hop counts must be params.
- **Slot 0 vs 15:** `doca_pcc_dev_user_algo` always reads slot-0 params/counters regardless of the negotiated `algo_slot`; non-ECE traffic (`ib_write_bw`) lands on slot 15 but still runs the algorithm. Push PPCC params to `algo_slot=0`. A future merged multi-algo binary must dispatch on `attr->algo_slot` instead.
- **PPCC port mapping quirk:** `mlxreg local_port=1` writes DPA port 0, but `mlx5_0` traffic arrives on DPA port 1 — algorithms therefore pin params/counters to port 0 on the device side; keep that pattern or runtime tuning silently no-ops.
- Dual-port operation is not viable (one ACTIVE PCC process per card + fabric stall on the second port); use one port per host.

## Conventions

- `grep -rn FIXME device/rp/<algo>/` lists the known unknowns; keep the FIXME-with-citation style (every constant cites a paper section or is flagged as a guess).
- Keep the per-algorithm README in sync with behavior changes — it is the design record (trace tables, branch semantics, tuning lessons like "retune `base_target` from a no-load fabric-RTT trace; idle RTT here is ~3–4 µs, not the paper's 25 µs").
- Wire encodings in contracts: time in ns (uint32), cwnd in FXP16 packets, gains in FXP16, rate in FXP20. Human-unit conversion is the orchestrator's job, not the device's.
