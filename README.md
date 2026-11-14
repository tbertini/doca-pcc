# doca-pcc — custom congestion control for BlueField-3

NVIDIA's DOCA PCC (Programmable Congestion Control) reference application, extended
with four RoCE congestion-control algorithms that run on the BlueField-3 DPA (Data Path
Accelerator, RISC-V cores on the NIC):

- **DCQCN** (ECN/CNP rate control)
- **TIMELY** (RTT-gradient rate control)
- **Swift** (absolute delay, window-based)
- **SMaRTT** (per-ACK ECN + delay, window-based; REPS is out of scope)

A fifth app, **multi**, loads all four at once and switches between them at runtime
through a register write, without restarting the process.

The host-side Linux program (`host/`) loads DPA kernels (`device/`) onto the NIC. The
DPA code receives per-flow congestion events (RTT probes, ACK/NACK, CNP, TX) and sets
each flow's TX rate. All algorithms are built into one `doca_pcc` binary and chosen at
start-up by a CLI flag.

The benchmark orchestrator that drives this code over SSH lives in a separate repo,
[incast-bench](https://github.com/tbertini/incast-bench).

## Repository layout and branches

This repo is the `applications/pcc` subtree of a DOCA 3.x SDK tree. On the BlueField
hosts it is checked out at `/opt/mellanox/doca/applications/pcc`.

| Branch / tag | Content |
|---|---|
| `main` | **The code to use.** All four algorithms plus the multi app. |
| `nvidia-baseline` (tag, commit `d3b33d6`) | NVIDIA's untouched reference application, for diffing. |
| `timely`, `dcqcn`, `smartt-reps`, `multi-algo`, … | Historical development and experiment branches. Everything needed is in `main`. |
| `swift-cx8` | Swift variant that actuates a credit window. Not runnable on BF3 (see [Swift README](device/rp/swift/README.md#where-the-credit-window-variant-went)). |

```
host/                    host program: argp flags, DOCA PCC setup, trace format strings
  pcc.c, pcc_core.c/.h
  <algo>_params_dump.c   prints an algorithm's PPCC contract as JSON (used by incast-bench)
device/
  pcc_common_dev.h
  rp/<algo>/             reaction-point algorithms: dcqcn, timely, swift, smartt_reps, multi
  rp/rtt_template/       NVIDIA's template algorithm (the no-flag default)
  rp/switch_telemetry/   NVIDIA sample, unchanged
  np/                    notification-point samples, unchanged
build_device_code.sh     dpacc invocation per app (called by meson)
meson.build
pcc_params.json          JSON-file equivalent of the CLI flags
tools/                   trace prettifier and PPCC register pretty-printer
```

Each `device/rp/<algo>/README.md` is the design record for that algorithm: the paper
mapping, fixed-point tricks, PPCC parameter and counter tables, trace formats, and
tuning lessons. Read it before changing that algorithm.

## Testbed

| Host | Role |
|---|---|
| `sgs-bluefield-01`, `sgs-bluefield-02` | BlueField-3 senders; build and run `doca_pcc` here |
| `hacc-u55c-04` | receiver (not a BlueField) |

Nothing in this repo compiles on a laptop: `dpacc`, the DOCA SDK and the hardware are on
the BlueField hosts. The usual flow is: edit locally, commit, push, then pull and build on
each BlueField. Both senders need the same build.

## One-time card setup

Programmable CC must be enabled in the NIC firmware configuration. It is already set on
the testbed cards; on a new card run this on the BlueField and then power-cycle it:

```bash
sudo mlxconfig -d /dev/mst/mt41692_pciconf0 s USER_PROGRAMMABLE_CC=1
```

## Build

On each BlueField host:

```bash
cd /opt/mellanox/doca/applications/pcc && git pull
cd /opt/mellanox/doca/applications
meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
ninja -C /tmp/build
```

- **Device code is compiled by `dpacc` during `meson setup`, not during `ninja`.** After
  any change under `device/` or to `build_device_code.sh`, re-run `meson setup --wipe`.
  A bare `ninja` silently reuses the stale device binary. Host-only changes rebuild
  with plain `ninja`.
- `--wipe` fails when `/tmp/build` does not exist yet, for example after a reboot. For
  the first build, drop `--wipe`.
- Outputs: `/tmp/build/pcc/doca_pcc` and `/tmp/build/pcc/<algo>_params_dump`.
  incast-bench expects exactly these paths.
- `<ALGO>_DEBUG=1 meson setup ...` (for example `SWIFT_DEBUG=1`) forces that algorithm's
  per-event traces on from boot. Normally they are compiled in but gated by the runtime
  `trace_on` PPCC parameter.

## Run

```bash
/tmp/build/pcc/doca_pcc --rp-swift -d mlx5_0 -l 70    # -l 70 prints device traces
```

| Flag | Algorithm | algo_id | Trace formats | README |
|---|---|---|---|---|
| `--rp-dcqcn` / `-rp-dc` | DCQCN | `0xBFFE` | 7–10 | [dcqcn](device/rp/dcqcn/README.md) |
| `--rp-timely` / `-rp-tm` | TIMELY | `0xBFFD` | 11–14 (13 retired) | [timely](device/rp/timely/README.md) |
| `--rp-swift` / `-rp-sw` | Swift | `0xBFFC` | 15–19 | [swift](device/rp/swift/README.md) |
| `--rp-smartt-reps` / `-rp-sr` | SMaRTT | `0xBFFB` | 20–24 | [smartt_reps](device/rp/smartt_reps/README.md) |
| `--rp-multi` / `-rp-mt` | all four + mux | `0xBFFA` (mux) | 25 | [multi](device/rp/multi/README.md) |
| *(no flag)* | NVIDIA `rtt_template` | `0xBFFF` | — | — |

Format 6 is a per-event summary shared by all algorithms. Formats 0–5 come from NVIDIA's
framework. Algo IDs and trace format IDs form one registry: never reuse an existing one.

Only **one ACTIVE PCC process per card** is possible (firmware HA). Stop the running
`doca_pcc` before starting another algorithm, or use `--rp-multi` to switch live.

In practice incast-bench starts and stops `doca_pcc` for you, for example
`python3 -m incast_bench.pccctl start sgs-bluefield-01 --flag=--rp-swift --expect-algo-id 0xBFFC --take-over`.
See its README.

## Runtime tuning (PPCC)

Each algorithm exposes its tuning parameters and counters through the standard PPCC
register, so sweeps need no rebuild:

- write a parameter vector: `mlxreg ... --reg_name PPCC ... cmd_type=0xB`
- read counters: `cmd_type=0xC`; read and clear: `cmd_type=0xD`
- push to `algo_slot=0` and `local_port=1` (see the constraints below)

The contract for each algorithm (parameter and counter indices, names, units, defaults,
bounds) lives in `device/rp/<algo>/algo/<algo>_params.h`. It is the single source of
truth, shared by the DPA code and the host-side `<algo>_params_dump` binary that
incast-bench reads. Parameter tables are append-only because orchestrators address them
by index, and the `trace_on` parameter always stays last.

`tools/` has a pretty-printer for raw `mlxreg` output and a TIMELY trace prettifier; see
[tools/README.md](tools/README.md).

## Adding an algorithm

Each algorithm is a self-contained `device/rp/<algo>/` tree:

- `rp_<algo>_dev_main.c`: DPA entry points `doca_pcc_dev_user_init`,
  `doca_pcc_dev_user_algo` and `doca_pcc_dev_user_set_algo_params`.
- `algo/<algo>.c/.h`: the algorithm.
- `algo/<algo>_ctxt.h`: per-flow state.
- `algo/<algo>_algo_params.h`: compile-time defaults, each with a short paper citation.
- `algo/<algo>_params.h`: the PPCC contract.

Wiring a new one touches `build_device_code.sh` (a new `elif` block), `meson.build` (a
dpacc stanza and the executable's dependency list), `host/pcc_core.h` (extern app
struct), `host/pcc_core.c` (flag, callback, and trace format strings indexed by format
ID, which must match the device's `doca_pcc_dev_trace_5` calls), and optionally a
`host/<algo>_params_dump.c` with its meson stanza.

There is no test suite. Verification means running on hardware and checking the device
traces (`-l 70`) and PPCC counters against the algorithm README's tables.

## BlueField-3 / DPA constraints

These are hard limits found on this hardware. Most design choices in the algorithm
READMEs follow from them.

- **Per-flow context is capped at 48 bytes.** Swift and SMaRTT use all 48.
- **No floating point, no libm.** Fixed point everywhere: FXP16 for cwnd, gains and
  ratios (`1<<16` = 1.0), FXP20 for rate (`1<<20` = 100% line rate). Transcendentals go
  through the `__dpa_fxp_log2` / `__dpa_fxp_pow2` intrinsics.
- **Rate is the only actuator.** The `credits` / `reload_credits` result fields are not
  backed on BF3. Window-based algorithms keep cwnd as their state and convert it to a
  rate with the BDP identity (see the Swift README).
- `doca_pcc_dev_get_ttl_hoplimit()` returns 0 on BF3, so hop counts are parameters.
- **Slot 0 vs 15.** `ib_write_bw` traffic does not negotiate through ECE and lands on
  slot 15, but still runs the algorithm. The single-algorithm apps always read slot 0's
  parameters and counters, so push PPCC writes to `algo_slot=0`. The multi app uses
  slots 0–4 instead, as described in its README.
- **PPCC port mapping.** `mlxreg local_port=1` addresses DPA port 0, while `mlx5_0`
  traffic arrives on DPA port 1. The device code therefore pins parameters and counters
  to port 0. Keep that pattern or runtime tuning silently does nothing.
- **One port per host.** Dual-port operation is not viable: only one PCC process can be
  active per card, and the second port stalls the fabric.

## Conventions

- Constants carry a short paper citation. Open questions and tuning seeds belong in the
  algorithm README, not in TODO comments.
- Wire encodings in the PPCC contracts: time in ns (uint32), cwnd in FXP16 packets, gains
  in FXP16, rate in FXP20. Converting to human units is the orchestrator's job.
- Keep the algorithm README in sync with behaviour changes. It is the design record.
- Idle fabric RTT on this testbed is about 3–4 µs, far below the papers' values. Retune
  delay targets from a no-load trace.
