# tools/ — host-side TIMELY/PPCC inspection helpers

Standalone scripts that run on the **BlueField host's Linux** (not the DPA, not
compiled into anything). They interpret `doca_pcc` trace output and `mlxreg`
PPCC register dumps into human-readable form. Nothing here is part of the meson
build.

| Tool | What it does |
|---|---|
| `timely-prettify.sh` | Translates `doca_pcc --rp-timely` trace numbers (`tsflow`, `ev_type`, `branch`, …) into readable names with optional ANSI color. Pipe a live `doca_pcc` run or a saved log through it. Understands the normalized `tsflow=0x<hex>` packing (formats 6/11/12/14) — unpacks it to `ts=<ns> flow=<tag>` and filters on the embedded flow tag via `--filter-flow`; older pre-normalization logs still parse. |
| `mlxreg-pretty.py` | Pretty-prints raw `mlxreg --reg_name PPCC` output: hex→decimal, plus a per-field hint. TIMELY-aware: labels `text[i]` cells with the param/counter name based on `cmd_type`. |

## Usage

```bash
# Live TIMELY trace, prettified:
/tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 | ./timely-prettify.sh

# Decode a PPCC register read:
mlxreg-pretty() { sudo mlxreg "$@" | python3 ~/mlxreg-pretty.py; }
mlxreg-pretty -d /dev/mst/mt41692_pciconf0 --reg_name PPCC -y --get \
    --indexes "lp_msb=0,pnat=0,local_port=1,target_app=0,algo_slot=0,algo_param_index=0" \
    --op "cmd_type=0xA"
```

## Deploying to the BlueField hosts

These are version-controlled here (single source of truth) but need to be
copied to each BlueField host to run. From a machine with SSH access:

```bash
for H in sgs-bluefield-01 sgs-bluefield-02; do
    scp tools/timely-prettify.sh tools/mlxreg-pretty.py "$H":~/   # user from ~/.ssh/config
done
```

## Staleness note

`mlxreg-pretty.py` hardcodes the TIMELY param/counter name lists
(`TIMELY_PARAMS`, `TIMELY_COUNTERS`). These mirror
[`device/rp/timely/algo/timely_params.h`](../device/rp/timely/algo/timely_params.h)
— the authoritative contract — but are **not** auto-derived. If you change the
header (add/reorder/rename params), update the script's lists to match.
