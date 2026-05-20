# MANUAL_VERIFY.md — TIMELY PPCC contract sanity check

After landing Phases 1–2 you have a `doca_pcc` binary that exposes 9 PPCC
parameters and 8 counters via the [`timely_params.h`](algo/timely_params.h)
contract. Before kicking off Phase 4 (`incast-bench` side), the user runs
the steps below by hand on `alveo-u50d-01` to confirm:

1. The DPA artifact builds and `doca_pcc` starts with the TIMELY algorithm
   on slot 0.
2. The contract is exposed to PPCC as expected (9 params, 8 counters, with
   the contract's defaults / mins / maxes).
3. The defaults baseline produces throughput close to the pre-PPCC
   baseline. If it diverges, this is where we reconcile the PLAN
   PLACEHOLDER defaults in `timely_params.h` against measured behavior.

The pre-PPCC baseline tag is `pre-ppcc-baseline` (at the commit *before*
Phase 1). If anything below regresses badly enough that we want a clean
slate: `git reset --hard pre-ppcc-baseline && rebuild`.

Throughout, replace `$MST` with your MST device path (commonly
`/dev/mst/mt41692_pciconf0` on BF3 — confirm via `sudo mst status -v`).

---

## 0. Pre-flight

```bash
# On the workstation:
git log --oneline -3      # confirm Phase 1 + Phase 2 commits land on HEAD

# On alveo-u50d-01 (the sender / RP):
sudo mst status -v        # confirm MST device, capture path
ls /opt/mellanox/mft/bin/mlxreg   # confirm mlxreg is on PATH
```

Build the DPA artifact and the host-side dumper:

```bash
# On alveo-u50d-01:
cd /path/to/doca/applications/pcc      # the working copy
meson setup /tmp/build -Denable_pcc=true --reconfigure   # if not configured
ninja -C /tmp/build doca_pcc timely_params_dump
```

Run the dumper and confirm the JSON matches what's in the header:

```bash
/tmp/build/applications/pcc/timely_params_dump | python3 -m json.tool | head -40
```

Expected: `"algo_id": 49149` (= 0xBFFD), 9 params with `idx 0…8`, 8 counters
with `idx 0…7`, names match `timely_params[]` / `timely_counter_names[]`.

---

## 1. Start `doca_pcc`

Use the existing invocation pattern (the same flags the user has been
running before this PR). For example:

```bash
sudo /tmp/build/applications/pcc/doca_pcc \
    -p <PCI_BDF> --probe-and-exit-off   # adjust to current pattern
```

In another shell, leave a tail on the `doca_pcc` log if your invocation
captures stdout (e.g., `tail -F /var/log/doca_pcc.log`).

---

## 2. PPCC contract sanity (read-only)

All `mlxreg` calls in this section are non-destructive. Run from the same
host where `doca_pcc` is running. Some MFT versions accept the
`--indexes` field unquoted; if your version complains, try quoting the
whole RHS or splitting into `--index_data`.

### 2.1 Algorithm slot info — `cmd_type=0x0`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=0" \
    --text_length=24
```

Expected: a "Value" line indicating algorithm 0 is loaded. `algo_id`
returned should equal `0xBFFD`.

### 2.2 Algorithm enable status — `cmd_type=0x3`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=3" \
    --text_length=4
```

Expected: enabled bit set, sl_bitmask non-zero. If disabled, enable with
`cmd_type=0x1` (see `mlxreg --reg_name=PPCC --help`).

### 2.3 Number of parameters — `cmd_type=0x4`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=4" \
    --text_length=4
```

Expected: **9**.

### 2.4 Number of counters — `cmd_type=0xE`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=14" \
    --text_length=4
```

Expected: **8**.

### 2.5 Per-parameter info — `cmd_type=0x5`

Loop over `algo_param_index=0…8` and confirm each returns the expected
default / max from `timely_params.h`:

```bash
for i in 0 1 2 3 4 5 6 7 8; do
    echo "=== param $i ==="
    sudo mlxreg -d $MST -y --get --reg_name PPCC \
        --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=$i,target_app=5" \
        --text_length=16
done
```

Cross-reference each output against the corresponding row in
[`algo/timely_params.h`](algo/timely_params.h)'s `timely_params[]` array.

### 2.6 Bulk get current params — `cmd_type=0xA`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=10" \
    --text_length=36
```

Expected (in order): `10000 80000 1049 52429 57344 3800 5 64 1048576`
(matching the contract defaults, which are aligned with `timely_algo_params.h`).

---

## 3. Counter sanity (no flows yet)

### 3.1 Number of counters — already validated in 2.4

### 3.2 Bulk get counters — `cmd_type=0xC`

```bash
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=12" \
    --text_length=32
```

Expected pre-flow: all 8 counters are 0 (no events dispatched yet).

---

## 4. Defaults-baseline parity (no measurement needed)

Phase 3 originally called for a `ib_write_bw` baseline comparison. That
step is **superseded**: the contract defaults in
[`algo/timely_params.h`](algo/timely_params.h) are now aligned with the
pre-PPCC hardcoded values in
[`algo/timely_algo_params.h`](algo/timely_algo_params.h), so a `doca_pcc`
instance that receives no param writes behaves identically to the
pre-Phase-1 build by construction.

Parity check: §2.6 above is the verification — `cmd_type=0xA` must
return `10000 80000 1049 52429 57344 3800 5 64 1048576`. If it does, the
contract is wire-equivalent to pre-PPCC and Phase 3 is done. The
orchestrator (incast-bench, Phases 4-7) will sweep parameters at runtime
via `cmd_type=0x8` / `0xB`; there is no fixed "best" value to converge to
here.

---

## 5. Reconciliation (historical, no longer needed)

This section described how to revert the original PLAN PLACEHOLDER
defaults to the pre-PPCC values. With §4 above superseded, this step is
unnecessary — the defaults are already aligned. Kept here for context in
case future contract edits diverge from `timely_algo_params.h` and a
similar parity check is needed.

---

## 6. Counter correctness (defaults or reconciled)

After a 10-second flow at the working defaults:

```bash
# Get-and-clear (cmd_type=0xD), so subsequent reads start fresh:
sudo mlxreg -d $MST -y --get --reg_name PPCC \
    --indexes "local_port=1,pnat=0,lp_msb=0,algo_slot=0,algo_param_index=0,target_app=13" \
    --text_length=32
```

Sanity checks:

- `events` (idx 0) > 0.
- `events` ≈ sum of (`md_gradient` + `md_thigh` + `ai` + `hai`) plus any
  non-RTT events (TX/NACK/CNP/new_flow); divergence is OK and expected to
  reflect the proportion of non-decision-branch events.
- `rtt_lt_tlow` should be ≤ `ai` (every RTT_LT_TLOW event increments AI;
  AI also fires on the gradient ≤ 0 branch when `n_factor == 1`).
- `rtt_gt_thigh` should equal `md_thigh` (1:1 by construction).
- `rate_clamped_min` should typically be 0 in a healthy flow at sane
  defaults; persistently non-zero counts indicate the rate is bouncing
  off the floor.

---

## 7. Rollback

If anything in this section indicates the Phase 1/2 changes are not safe
to keep, the rollback is:

```bash
git reset --hard pre-ppcc-baseline
meson setup /tmp/build -Denable_pcc=true --reconfigure
ninja -C /tmp/build doca_pcc
# Redeploy the rebuilt binary to alveo-u50d-01 if needed.
```

After rollback, the `incast-bench` side is unaffected because it is not
yet PPCC-aware (Phases 4–7 are the next change set).
