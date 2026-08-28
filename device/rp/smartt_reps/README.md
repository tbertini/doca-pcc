# SMaRTT (smartt-reps) — sender-based ECN+delay windowed congestion control for DOCA PCC

Implementation of **SMaRTT** from Bonato et al., *"SMaRTT-REPS: Sender-based Marked
Rapidly-adapting Trimmed & Timed Transport with Recycled Entropies"*, arXiv:2404.01630
(Apr 2024). SMaRTT is a sender-based, **window-based** congestion controller that
combines **delay (RTT)** and **ECN** signals per ACK (§3.2, four-case control loop),
reacts to severe congestion with **QuickAdapt** (§3.3, Algorithm 2), and reclaims free
bandwidth with **FastIncrease** (§3.4, Algorithm 3), gated by **Wait-to-Decrease**
(§3.6.1).

This document is the **design spec** (written before the code; kept in sync after).
Registry: algo_id **0xBFFB**, trace formats **20–24**, CLI flag `--rp-smartt-reps`,
branch `smartt-reps` off `main`.

## REPS is intentionally NOT implemented

REPS (§3.6, Algorithm 4) is per-packet adaptive load balancing: the sender picks each
packet's ECMP entropy (e.g. IPv6 flow label) per transmission and the receiver echoes it
on ACKs. A DOCA PCC reaction-point algorithm on BlueField-3 controls exactly one
actuator — the per-flow FXP20 TX rate — and exposes no per-packet entropy hook (verified
against `doca_pcc_dev.h` / `doca_pcc_dev_event.h`: `doca_pcc_dev_results_t` carries only
`rate`, `rtt_req`/`probe_req`, `probe_type_slot`, and the BF3-unbacked credit fields).
Additionally this testbed (2 senders → 1 receiver through one switch) has no meaningful
multipath for entropy recycling to exploit. The branch keeps the paper's full name for
identification; the CC algorithm implemented is SMaRTT.

## File layout

```
device/rp/smartt_reps/
  rp_smartt_reps_dev_main.c     DPA entry points (user_init / user_algo / set_algo_params)
  algo/smartt_reps.c/.h         the algorithm (init / event handlers / param setter)
  algo/smartt_reps_ctxt.h       per-flow context (exactly 48 bytes — hard cap)
  algo/smartt_reps_algo_params.h  compile-time defaults, FIXME-with-citation style
  algo/smartt_reps_params.h     THE PPCC contract (single source of truth)
host/smartt_reps_params_dump.c  no-DOCA-deps contract dumper (JSON for incast-bench)
```

## Signal mapping (paper → BlueField-3) — all header facts verified

| Paper signal | BF3 reality (verified in `~/Desktop/doca/include/doca_pcc_dev_event.h`) | Adaptation |
|---|---|---|
| per-ACK `p.rtt` | Data ACKs carry no RTT. RTT **probe** events (`DOCA_PCC_DEV_EVNT_RTT`, requested via `results->rtt_req=1`) carry t1/t2/t4 timestamps + event ts (t6) | Keep last `fabric_rtt = (t6−t1) − (t4−t2)` in ctxt (sync-free; swift's exact formula + t2/t4==0 fallback). ACK decisions read the stored value. Swift's probe re-arm + TX abort machinery ported verbatim |
| per-ACK `p.ecn` | ECN marks never reach the sender as ACK bits; the RoCE receiver emits **CNP** events (`DOCA_PCC_DEV_EVNT_ROCE_CNP` = 2) for marked arrivals | `p.ecn := (now − last_cnp_ts) ≤ ecn_cnp_hold_ns` (param). CNPs are receiver-rate-limited, not 1:1 with marks — WTD α/threshold are the compensating tuning levers |
| per-ACK `p.size` | ACK/NACK/CNP events carry `first_sn` (first coalesced event's serial number) + `num_coalesced` (16-bit). **No byte count.** (`doca_pcc_dev_get_rtt_np_rx_byte_counter()` is hardcoded 0 on BF3 — no cross-check exists) | `p.size` in **packets** = wrap32 ΔPSN of `first_sn` between successive ACK/NACK events. First event or anomaly (Δ > max(2·cwnd_pkts + 2, 256)): fall back to `num_coalesced`, itself bounded by the same cap, and bump the `psn_anomaly` counter. PSN semantics verified on HW via trace fmt 23 |
| packet trimming | Not available on this fabric. Paper's own fallback: timeouts (§3.3, §4.2 — "at most ~2 base RTTs" cost, and **double `md`** in timeout mode) | **NACK events** (`DOCA_PCC_DEV_EVNT_ROCE_NACK` = 5) are the loss/severe-congestion trigger → Algorithm 1 lines 28–34 (trimmed/timeout branch) |
| QuickAdapt `acked` over last trtt | No receiver byte feedback | Per-flow ΔPSN accumulator in ctxt, reset each trtt epoch (Algorithm 2 line 12) |
| `unacked` → `bytes_to_ignore` | In-flight bytes not exposed to PCC | `bytes_to_ignore ≈ cwnd` (one window), in packets |
| cwnd actuation | `rate` is the **only** actuator (credits unbacked on BF3) | BDP identity at the **live** pacing RTT, exactly like swift: `rate_fxp20 = clamp((cwnd_fxp16 << 20)/bdp_fxp16(pacing_rtt), min_rate, MAX)` with `bdp_fxp16(rtt) = line_rate_mbps·rtt·2^16/(mss_bytes·8000)` and `pacing_rtt = clamp(last_rtt_ns, brtt, 100 ms)`. The paper's `bdp` (equation denominators, cwnd cap, γ) stays at `brtt`. |
| `brtt` / `trtt` | Idle fabric RTT on this testbed ≈ **3.8 µs** (timely/swift lesson — not the paper's 12 µs) | `base_rtt_ns` param (seed 4000), `target_rtt_ns` param (seed 6000 = 1.5×, §3.5). Retune from a no-load trace |

**First-consumer warning:** every existing algorithm in this repo masks or ignores ACK
events (`rtt_template` and `dcqcn` set `disable_event_bitmask = 1 <<
DOCA_PCC_DEV_EVNT_ROCE_ACK`). SMaRTT is the first ACK-event consumer on this hardware —
`doca_pcc_dev_user_init` must **not** mask ACKs, and Phase-2 smoke must confirm ACK
events actually arrive (`ack_events` counter + fmt 23). Event coalescing
(`num_coalesced`) bounds the DPA event rate; paper Fig. 3b shows reacting once per ≤50
packets costs <5%.

## Units and fixed-point conventions

**cwnd is FXP16 packets** (1<<16 = 1.0 packet = 1 MSS/MTU). Rationale: ΔPSN natively
counts packets; every SMaRTT formula (Eq. 1–4, QA, FastIncrease) scales uniformly by MSS
on a fixed-MTU RoCE fabric, so byte units divide out; and swift's hardware-validated
cwnd→rate BDP actuation is reused unchanged. In packet units, `mtu ≡ 1.0` (65536) and
`bdp ≡ bdp_fxp16` above. Wire encodings follow the repo contract style: time ns
(uint32), cwnd/gains FXP16, rate FXP20. Fractional cwnd matters: FI adds
`(p.size/cwnd)·fi` ≈ 0.02 pkt per event — integer packets would round it to zero.

All arithmetic: unsigned 32-bit state, **64-bit intermediates for every multiply**, no
float/sqrt/libm (SMaRTT needs no transcendentals — all ratios are integer divides; this
is simpler than swift). All timestamp math uses wrap32-safe diffs
(`smartt_reps_diff_with_wrap32`, same as swift). Saturating adds for the uint16
accumulators.

## Control law (Algorithm 1, §3.2–3.6.1 — implementation contract)

Per **ACK event** (`DOCA_PCC_DEV_EVNT_ROCE_ACK` = 4), with `pkts_acked` = ΔPSN packets
(the paper's `p.size`), `num_coalesced` = the HW coalescing count, `rtt` = stored
fabric_rtt, `ecn` = CNP-hold predicate:

```
acked  += pkts_acked  (sat16)     # Alg1 line 4
ignored += pkts_acked (sat16)     # Alg1 line 5   (bytes_ignored, in pkts)
if ignored < ignore_target:       # Alg1 lines 8-10: post-QuickAdapt blackout (§3.3)
    counter[ACKS_IGNORED]++ ; emit fmt20(branch=BLACKOUT) ; return (rate unchanged)
avg_wtd = α·(ecn?1:0) + (1−α)·avg_wtd          # WTD §3.6.1, once per ACK event
can_decrease = (avg_wtd ≥ wtd_thresh)          # thresh=0 disables WTD gating
qa_fired      = quick_adapt(now)                          # Alg2 (below) — always called, may fire
fastinc_fired = fast_increase(pkts_acked,num_coalesced)   # Alg3 (below) — always called, may add k·mtu·num_coalesced
if qa_fired or fastinc_fired: goto clamp   # Alg1 lines 15-17 (deviation: we still clamp+actuate)
if      ecn and rtt ≤ trtt and can_decrease:  FD          # §3.2.1 Eq.1
elif    ecn and rtt > trtt and can_decrease:  MD (+FD)    # §3.2.2 Eq.2 ("additionally applies FD")
elif  ! ecn and rtt > trtt:                   FI          # §3.2.3 Eq.3
elif  ! ecn and rtt ≤ trtt:                   MI (+FI)    # §3.2.4 Eq.4 ("followed by FI")
else: (ecn but !can_decrease)  counter[WTD_BLOCKED]++     # no window change
clamp:  cwnd = clamp(cwnd, min_cwnd, fxp_mult(max_cwnd_frac, bdp_fxp16))   # Alg1 line 36 + §3.5
actuate: pacing_rtt = clamp(last_rtt_ns, brtt, 100 ms)      # swift's pacing law
         rate = clamp((cwnd << 20)/bdp(pacing_rtt), min_rate, DOCA_PCC_DEV_MAX_RATE); store; results
```

The four window updates in FXP16-packet units (`cwnd` FXP16, `size` integer pkts,
`bdp` = bdp_fxp16, `mtu` = 1.0 = 65536; every product via uint64):

```
FD:  cwnd -= ((u64)cwnd · fd / bdp) · size                       # Eq.1  (cwnd/bdp)·fd·size
MD:  dec   = ((u64)(rtt−trtt) << 16) / rtt                        # ratio FXP16, rtt>trtt>0
     dec   = fxp_mult(dec, md) · size                             # FXP16 pkts
     cwnd -= min(dec, size << 16)                                 # Eq.2 cap: at most size
     then apply FD as well                                        # §3.2.2 text
FI:  cwnd += fxp_mult( ((u64)size << 32) / cwnd , fi·γ )          # Eq.3  (size/cwnd)·mtu·fi
MI:  inc   = ((u64)(trtt−rtt) << 16) / rtt                        # ratio FXP16
     inc   = min-with (size<<16) of fxp_mult3(inc, (size<<32)/cwnd, mi·γ) # Eq.4
     cwnd += inc ; then apply FI as well                          # §3.2.4 text
```

`fi` and `mi` (param or derived) are multiplied on device by γ = bdp/reference_bdp
(§3.5 "Scaling", reference = 150000 bytes = 100 Gbps × 12 µs); `fd`/`md` are not.

All decreases underflow-guard (`dec ≥ cwnd → cwnd = min_cwnd` before clamp). `rtt == 0`
cannot reach MD/MI ratios (flow init stores `rtt = brtt > 0`).

**On NACK event** (Algorithm 1 lines 28–34; NACK = trimming/timeout surrogate):

```
(update PSN tracker: NACK first_sn = receiver's expected PSN ⇒ ΔPSN counts delivered
 pkts; acked += Δ, ignored += Δ — keeps QA's acked measurement honest under go-back-N)
cwnd -= 1 MTU (65536)             # line 29, p.size of one lost packet; underflow-guard
trigger_qa = true                 # line 30
if ignored ≥ ignore_target: quick_adapt(now)   # lines 32-34
clamp + actuate as above; rtt_req = 0
```

**QuickAdapt (Algorithm 2)** — called per ACK (and NACK as above); at most one fire per
trtt epoch:

```
adapted = false
if wrap_diff(now, qa_epoch_start) ≥ trtt:      # "now ≥ end"
    if trigger_qa and qa_epoch_valid:          # "end ≠ 0": no fire before first full epoch
        trigger_qa = false; adapted = true
        ignore_target = old_cwnd >> 16 (round up, ≥1)  # Alg2 line 8: bytes_to_ignore = unacked ≈ pre-fire cwnd
        ignored = 0
        cwnd = sat_u32( (max(acked,1) << 16) · qa_scaling >> 16 )  # max(acked,mtu)·qa_scaling, u64 math
        cwnd = clamp(cwnd, min_cwnd, max_cwnd_frac·bdp)      # caller clamps again; idempotent
        counter[QA_FIRED]++ ; emit fmt21
    qa_epoch_start = now; qa_epoch_valid = 1
    acked = 0                                  # epoch accumulator reset (line 12)
return adapted
```

**FastIncrease (Algorithm 3)** — `rtt ≈ brtt` means `rtt ≤ fxp_mult(brtt,
fast_inc_thresh)`:

```
if rtt ≤ brtt·fast_inc_thresh and !ecn:
    count += pkts_acked (sat16)
    if (count << 16) > cwnd or fi_active:
        cwnd += (u64)fast_inc_k · num_coalesced   # k MTUs per coalesced ACK arrival
        fi_active = true; counter[FAST_INC]++ ; emit fmt22 ; return true
    return false                               # still accumulating: four cases run
else: count = 0; fi_active = false; return false
```

**On RTT event**: store `fabric_rtt` (fallback `total_rtt` if t2/t4 unpopulated), reset
abort state, re-arm `rtt_req = 1`, rate unchanged. The ACK loop consumes the stored
value — SMaRTT does not adjust cwnd on probe events (the paper has no probe events; this
is the central cadence adaptation, see Deviations).

**On CNP event**: `last_cnp_ts = now`, `cnp_seen = 1`, `counter[CNP_EVENTS]++`, rate
unchanged (decreases happen in the ACK loop through `p.ecn`).

**On TX event**: swift's RTT-probe abort/retry machine verbatim (300 µs base, exponential
backoff). No cwnd change.

**New flow** (`cur_rate == 0` sentinel): `cwnd = init_cwnd` (param 0 ⇒ use `bdp_fxp16`),
`rtt = brtt`, `avg_wtd = 0`, PSN/QA/FI state invalid/zero, actuate, `rtt_req = 1`.

## Per-flow context — 48-byte budget (hard cap, exactly 48)

```c
typedef struct {
    uint32_t cur_rate;        /* FXP20 actuated rate; 0 = new-flow sentinel   [4]  */
    uint32_t cwnd_fxp16;      /* control variable, FXP16 packets              [4]  */
    uint32_t last_rtt_ns;     /* latest fabric RTT from probe; init = brtt    [4]  */
    uint32_t last_cnp_ts;     /* CNP arrival ts for the ECN-hold predicate    [4]  */
    uint32_t qa_epoch_start;  /* QuickAdapt trtt-epoch start ("end" anchor)   [4]  */
    uint32_t last_ack_psn;    /* PSN tracker for ΔSN acked-packets            [4]  */
    uint32_t probe_start_ts;  /* RTT-probe abort machine (swift probe_start_ts)  [4]  */
    uint16_t acked_pkts;      /* acked in current trtt epoch (saturating)     [2]  */
    uint16_t packets_to_ignore;     /* bytes_to_ignore, packets                     [2]  */
    uint16_t ignored_pkts;    /* bytes_ignored, packets (saturating)          [2]  */
    uint16_t avg_wtd_fxp16;   /* WTD EWMA, capped 65535 (≈1.0)                [2]  */
    uint16_t fast_inc_count;  /* FastIncrease byte counter, packets (sat)     [2]  */
    uint8_t  flags;           /* bit0 trigger_qa  bit1 fi_active  bit2 psn_valid
                                 bit3 qa_epoch_valid  bit4 cnp_seen
                                 bit5 rtt_probe_outstanding                   [1]  */
    uint8_t  abort_cnt;       /* probe abort backoff exponent                 [1]  */
    uint8_t  tx_since_probe_req; /* TX events between req and SENT flag      [1]  */
    uint8_t  reserved8;       /*                                              [1]  */
    uint16_t reserved16;      /*                                              [2]  */
    uint32_t reserved32;      /* pad to exactly 48                            [4]  */
} cc_ctxt_smartt_reps_t;      /* 7·4 + 5·2 + 4·1 + 2 + 4 = 48 */
```

(Paper claims 19 B/flow; the extra is the RTT-probe machinery, CNP hold state, and the
rate cache that BF3's probe-driven, rate-actuated environment requires.)

## PPCC contract (FROZEN — append-only; `trace_on` stays last)

Params (index = wire index; every default cites a paper section or is a FIXME seed):

| idx | name | unit | default | min | max | source |
|---|---|---|---|---|---|---|
| 0 | `base_rtt_ns` | ns | 4000 | 500 | 500000 | brtt; measured fabric ≈3.8 µs (NOT paper's 12 µs) |
| 1 | `target_rtt_ns` | ns | 6000 | 600 | 750000 | trtt = 1.5·brtt (§3.5) |
| 2 | `fd` | FXP16 | 52429 | 0 | 655360 | 0.8 (§3.5, tuned 100G/4KiB — matches this fabric) |
| 3 | `md` | FXP16 | 262144 | 0 | 1310720 | 4.0 = §3.2.2's 2.0 doubled for timeout mode (§3.3) |
| 4 | `fi` | FXP16 | 16384 | 0 | 655360 | 0.25 (§3.5 reference value); γ-scaled on device, note below |
| 5 | `mi` | FXP16 | 0 | 0 | 1310720 | 0 ⇒ derive brtt/(trtt−brtt) on device (§3.2.4; =2.0 at 1.5×) |
| 6 | `qa_scaling` | FXP16 | 52429 | 1 | 131072 | 0.8 (§3.5) |
| 7 | `fast_inc_k` | FXP16pkt | 131072 | 0 | 655360 | k = 2 MTU per ACK (§3.5) |
| 8 | `fast_inc_thresh` | FXP16 | 72090 | 65536 | 262144 | rtt≈brtt tolerance ×1.1 — FIXME: guess, paper says "close to" |
| 9 | `wtd_alpha` | FXP16 | 8192 | 0 | 65536 | α = 0.125 — FIXME: paper never gives α (§3.6.1) |
| 10 | `wtd_thresh` | FXP16 | 16384 | 0 | 65536 | 0.25 (§3.6.1); 0 disables WTD gating |
| 11 | `ecn_cnp_hold_ns` | ns | 10000 | 0 | 10000000 | FIXME: CNP→ECN assertion window; guess ≳1 trtt |
| 12 | `max_cwnd_frac` | FXP16 | 81920 | 16384 | 655360 | 1.25·BDP cap (§3.5; Alg1 line 36 shows 1.0 — text wins, tunable) |
| 13 | `min_cwnd` | FXP16pkt | 65536 | 66 | 6553600 | 1 MTU floor (§3.5) |
| 14 | `init_cwnd` | FXP16pkt | 0 | 0 | 134217728 | 0 ⇒ BDP |
| 15 | `mss_bytes` | bytes | 4096 | 256 | 9000 | segment size for BDP; FIXME verify RoCE MTU in use |
| 16 | `line_rate_mbps` | Mbps | 100000 | 1000 | 800000 | BDP input |
| 17 | `min_rate` | FXP20 | 64 | 1 | 104858 | rate floor (repo convention, = timely/swift) |
| 18 | `trace_on` | bool | 0 | 0 | 1 | diagnostics only — KEEP LAST |

γ note (§3.5 "Scaling"): the paper multiplies `fi`/`mi` by γ = bdp/reference_bdp
(reference = 150000 bytes = 100 Gbps × 12 µs). The device applies γ at use time
(`smartt_reps_gamma_fxp16`, from `line_rate_mbps`/`base_rtt_ns`), so the `fi`/`mi`
params hold the paper's reference values and the effective gains track the configured
fabric (γ ≈ ⅓ at the defaults: effective fi ≈ 0.083, mi ≈ 0.67). `fd`/`md` are fixed
regardless of link speed (paper: always react strongly).

Counters:

| idx | name | meaning |
|---|---|---|
| 0 | `events` | total events dispatched |
| 1 | `ack_events` | ACK events (the control-loop driver — must be nonzero!) |
| 2 | `rtt_events` | RTT probe responses |
| 3 | `cnp_events` | CNP events (the "is ECN marking live?" answer) |
| 4 | `nack_events` | NACK events (loss signal) |
| 5 | `fd_applied` | Fair Decrease applications (incl. the FD inside MD) |
| 6 | `md_applied` | Multiplicative Decrease applications |
| 7 | `fi_applied` | Fair Increase applications (incl. the FI inside MI) |
| 8 | `mi_applied` | Multiplicative Increase applications |
| 9 | `qa_fired` | QuickAdapt window resets |
| 10 | `fast_inc_acks` | ACK events in active FastIncrease |
| 11 | `wtd_blocked` | ECN-decrease wanted but avg_wtd < thresh |
| 12 | `acks_ignored` | ACK events swallowed by post-QA blackout |
| 13 | `rate_clamped_min` | actuated rate hit min_rate |
| 14 | `rate_clamped_max` | actuated rate hit MAX_RATE |
| 15 | `psn_anomaly` | ΔPSN sanity-cap fallbacks to num_coalesced |

## Trace formats 20–24 and 6 (gated by `trace_on`, swift-style `*_TRACE_ENABLED`)

Host `pcc_core.c` format strings must match these argument orders exactly.

**Trace normalization (2026-07-10):** every SMaRTT trace call (format 6 and 20-24) now
carries `tsflow` as `arg1`, and every format that has a rate field puts it last. This
brought the branch in line with the same normalization pass on timely/dcqcn/swift: a
fixed arg1 makes cross-format correlation trivial (join on flow_tag, or eyeball the
timestamp half), and rate-last is now a repo-wide convention, not just a SMaRTT one.
**Only trace-call arguments/strings/docs changed — no algorithm/control-law logic moved.**

### `tsflow` (arg1, everywhere)

```c
#define SMARTT_REPS_TRACE_TSFLOW(ts, flow) \
	((((uint64_t)(uint32_t)(ts)) << 32) | (uint64_t)(uint32_t)(flow))
```

| bits | field | source |
|---|---|---|
| `63:32` | `ts_ns` | event timestamp, `doca_pcc_dev_get_timestamp(event)`; wraps every 2^32 ns ≈ 4.295 s |
| `31:0` | `flow_tag` | `doca_pcc_dev_get_flowtag(event)` |

Printed as a single hex field, `tsflow=0x%lx` (split the two 32-bit halves host-side if
needed). Defined once in `smartt_reps.h` (next to `SMARTT_REPS_TRACE_ENABLED`, since both
`rp_smartt_reps_dev_main.c` — format 6 — and `smartt_reps.c` — formats 20-24 — need it
and both already include that header). `ts`/`flow` are always exactly these two getters,
called on the current event — never re-derived, cached, or approximated.

### BEFORE → AFTER map

| fmt | BEFORE args (old string) | AFTER args (new string) | dropped field / rationale |
|---|---|---|---|
| 6 | `ev_type, flow_tag, port, algo_slot, rate` | `tsflow, ev_type, port, algo_slot, rate` | `flow_tag` folded into tsflow's low 32 bits |
| 20 ack | `branch, rtt, cwnd, rate, acked_pkts` | `tsflow, branch, rtt, cwnd, rate` | `acked_pkts` dropped — the coalesced-ACK size is already visible on fmt 22 (`cnt_ncoal` low half) and fmt 23 (`delta_ncoal` high half); a third copy on every ack/blackout/new-flow emission was redundant |
| 21 qa | `acked_epoch, new_cwnd, packets_to_ignore, rate, src` | `tsflow, src, packets_to_ignore, new_cwnd, rate` | `acked_epoch` dropped — it is exactly the running sum of fmt-23's `delta_ncoal` high half over the trtt epoch, so it is reconstructible off-device from data already traced |
| 22 fastinc | `count, cwnd, rate, rtt, ncoal` | `tsflow, cnt_ncoal, rtt, cwnd, rate` | no field dropped — `count` (`fast_inc_count`) and `ncoal` packed together into `cnt_ncoal` to make room for tsflow at a fixed 5-arg budget |
| 23 sig | `first_sn, sn, delta_pkts, ncoal, avg_wtd` | `tsflow, first_sn, delta_ncoal, avg_wtd, 0` | `sn` (`doca_pcc_dev_get_sn(event)`) dropped — for one coalesced event `sn ≈ first_sn + delta_pkts`, so it carries no information beyond `first_sn` + `delta_ncoal`'s high half, which are both still traced; `delta_pkts` and `ncoal` packed into `delta_ncoal`. This format has no rate field, so the 5th slot is an unused `0` placeholder (rate-last doesn't apply — nothing to put last) |
| 24 loss | `first_sn(psn), cwnd, rate, ignored_pkts, trigger_qa` | `tsflow, first_sn, ign_qa, cwnd, rate` | no field dropped — `ignored_pkts` and `trigger_qa` packed together into `ign_qa` to make room for tsflow |

Every AFTER string above is byte-identical to what `rp_smartt_reps_dev_main.c` /
`smartt_reps.c` emit post-normalization. Format 6's AFTER string is additionally
byte-identical across the timely/dcqcn/swift/smartt-reps branches (it is one shared slot
in the merged `trace_message_formats[]` table host-side).

### Packed sub-fields (bit ranges, all printed `0x%lx`)

| fmt | field | bits | contents |
|---|---|---|---|
| 22 | `cnt_ncoal` | `[31:16]` | `fast_inc_count` — uint16 saturating FastIncrease byte/packet counter |
| 22 | `cnt_ncoal` | `[15:0]` | `ncoal` — `num_coalesced` (16-bit HW field) |
| 23 | `delta_ncoal` | `[31:16]` | `delta_pkts` — the ΔPSN-derived acked-packet count for this event (the `pkts_acked` local) |
| 23 | `delta_ncoal` | `[15:0]` | `ncoal` — `num_coalesced` |
| 24 | `ign_qa` | `[31:1]` | `ignored_pkts` — bytes-ignored-in-packets since the last QuickAdapt fire |
| 24 | `ign_qa` | `[0]` | `trigger_qa` — 0/1, NACK-armed "fire QuickAdapt at next epoch boundary" flag |

Cast-before-shift, not shift-before-cast: `fast_inc_count` and `delta_pkts`(`size`) feed
the `<<16` half of `cnt_ncoal`/`delta_ncoal`. `fast_inc_count` is `uint16_t`, narrower
than `int`, so it integer-promotes to *signed* `int` before any `&`/`<<`; masking then
shifting a promoted `int` left by 16 overflows into the sign bit for counts above 32767 —
undefined behavior in C. The device code widens to `uint64_t` **before** the shift
(`(uint64_t)(ctx->fast_inc_count & 0xFFFF) << 16`), producing the identical bit pattern
with no UB — the same idiom `smartt_reps_fast_increase` already used for `count_fxp16`
one line above it. `delta_pkts`/`size` is already `uint32_t` at its call site (no
promotion happens either way), but is written the same way for consistency. `ign_qa`'s
`ignored_pkts` (also `uint16_t`) is cast to `uint64_t` before its `<<1` for the same
reason.

`branch` (fmt 20): low nibble 0=FD 1=MD 2=FI 3=MI 4=wtd_blocked 5=blackout 6=fastinc
7=qa_adapted; OR-ed high bits: 0x80 rate clamped, 0x40 ecn asserted, 0x20 can_decrease.
Unchanged by the normalization pass.

Format 6 (shared per-event summary) is also emitted from dev_main under the same gate,
like swift/timely; it now carries `tsflow` as arg1 (see BEFORE → AFTER map above).

## Deviations from the paper (complete list)

1. **REPS omitted** (see top).
2. **p.rtt is the last probe measurement**, not per-ACK: BF3 ACKs carry no timestamps.
   Probe cadence is hardware-determined (measure in Phase 2; if ~ms, the delay signal is
   quasi-static between probes and ECN/QA carry the fast path — the four-case loop
   still behaves correctly, just with a slow delay input).
3. **p.ecn is a CNP-hold predicate**, not a per-packet mark bit; WTD's EWMA steps once
   per ACK event, not per packet. α/thresh/hold are the adaptation levers.
4. **NACK replaces trimming/timeout** as the QuickAdapt trigger; the paper doubles `md`
   in timeout mode (§3.3), so the default is 4.0. No retransmit control from PCC (RoCE
   go-back-N does that in hardware); Alg1's `retransmit_packet(p)` is a no-op here.
5. **acked bytes ≈ ΔPSN × MSS** from `first_sn` deltas (coalesce-group first-to-first
   telescopes correctly); `num_coalesced` fallback on anomalies.
6. **`bytes_to_ignore = pre-fire cwnd`** (paper: `unacked`, not exposed on BF3; the
   in-flight bytes at fire time ≈ the old window).
7. **QA/FastIncrease returns still clamp+actuate** (paper's early returns skip line 36's
   clamp; QA/FI values are within sane bounds anyway — safety, no behavioral change).
8. **cwnd floor on decreases**: underflow-guarded to `min_cwnd` (1 MTU) per §3.5; the
   paper's pacer-below-1-MTU option is not implemented (incast degree here is 2).
9. **FastIncrease "per ACK"** scaled by `num_coalesced` (one event may carry many ACKs).

## Hardware validation (2026-07-05, alveo-u50d-01/02 → alveo-u55c-03, 2:1 incast)

Baseline with the compiled-in (paper-default) vector: **97.32 Gb/s aggregate** over
10 s; two 20 s confirmation runs: **97.31 / 97.32 Gb/s**. A 13-vector single-axis PPCC
sweep around the anchor (`incast-bench --sweep smartt-reps-grid1.json`) measured
97.32 Gb/s on **every** vector — the aggregate is receiver-port-saturated across the
sane parameter plane, so the paper defaults are the shipped defaults (no retune needed
for this objective). Per-sender caps only become visible in the aggregate below the
~48.7 Gb/s fair share on this 2:1 topology. (This run predates the 2026-08-26
faithfulness fixes: `md` default 2.0→4.0, on-device γ scaling, pre-fire-cwnd QA
blackout. It also predates the 2026-08-27 switch to swift's live-RTT pacing
denominator; swift's own A/B (see its README) predicts fewer marks and shallower
queues at near-identical aggregate. Given the flat plane above, no impact on the
aggregate is expected.)

Hardware truths established (per-vector PPCC counters, 10 s runs, per sender):

- **ACK events fire on BF3** (first consumer in this repo): ~2.97 M ACK events / 10 s,
  ~5 packets coalesced per event. The whole control loop is live.
- **RTT probe cadence ≈ 107 µs** (~94 k probe responses / 10 s) — the stored-delay
  adaptation refreshes far faster than feared.
- **Loaded fabric RTT exceeds 15 µs** (idle ≈ 3.8 µs): even `target_rtt_ns=15000`
  never flipped the loop out of the `rtt > trtt` half. Under saturation the algorithm
  therefore lives in the **FI-dominant regime** (`!ecn ∧ rtt>trtt` ⇒ Fair Increase on
  ~every ACK, cwnd riding the 1.25·BDP cap, rate clamped at MAX).
- **ECN/CNP marking is live but sparse**: ~90 CNPs / 10 s vs ~3 M ACKs — the switch
  marks only rare bursts at this load. With the default WTD threshold the EWMA never
  reaches 0.25, so `fd/md_applied = 0`; setting `wtd_thresh=0` made the same CNPs
  produce 36 MD (+FD) applications — the full ECN→CNP→hold→decrease path works and is
  simply gated off by WTD at this marking rate. Tune `wtd_thresh`/`wtd_alpha`/
  `ecn_cnp_hold_ns` on a fabric that actually marks.
- **Actuation proof**: `max_cwnd_frac=0.75` drove `rate_clamped_max` from ~3 M to
  exactly 0 (window bound below BDP ⇒ rate never hits MAX) while QA-scaling changes
  modulated the post-QA blackout length (`acks_ignored` 303/462/509 at 0.5/0.8/1.0) —
  every PPCC param demonstrably reaches the running algorithm.
- QuickAdapt: `qa_fired == nack_events` (185) exactly — every NACK burst triggered one
  epoch-gated QA; blackouts averaged ~2.4 ACK events (≈ one cwnd of ~15 pkts at ~5
  pkts/event). `psn_anomaly` ≈ 96 / 2.97 M (0.003 %) — the ΔPSN acked-bytes model holds.

## Bring-up watchlist (Phase 4)

- `ack_events == 0` ⇒ ACK events masked/not firing — the whole loop is dead. Check
  `disable_event_bitmask` and firmware event config.
- `cnp_events == 0` under load ⇒ switch isn't ECN-marking (or CNP path off): SMaRTT
  degrades to delay-only — FD/MD never fire, MI/FI + QA-on-NACK carry the algorithm.
  Legitimate mode; document which reality this fabric is in, tune accordingly.
- `wtd_blocked` ≫ `md_applied`+`fd_applied` while queues grow ⇒ CNP rate-limiting starves
  the EWMA below thresh: lower `wtd_alpha`'s denominator effect (raise α), lower
  `wtd_thresh`, or lengthen `ecn_cnp_hold_ns`.
- `qa_fired` every epoch ⇒ NACK storm; window cap or target too high.
- `psn_anomaly` growing ⇒ ΔSN model wrong — check fmt 23 (`first_sn` deltas across
  consecutive events against `delta_ncoal`'s bits[31:16]; the raw event `sn` is no longer
  traced as of the trace-format normalization, see Trace formats section).
- All-MI, no decreases, RTT flat at ~brtt ⇒ target too high to ever bind (retune from
  no-load trace, like timely's base_target lesson).

## PPCC / runtime tuning

Same interface as timely/swift: `mlxreg -d <MST> -y --reg_name PPCC --set/--get` with
`cmd_type=0xB` (bulk set), `0xC/0xD` (counters), `local_port=1`, `algo_slot=0`.
Device pins params/counters to **DPA port 0, slot 0** (the port-mapping quirk:
`local_port=1` writes DPA port 0 while mlx5_0 traffic arrives on DPA port 1).
`smartt_reps_params_dump` emits this contract as JSON for incast-bench.

## Hardware-mandated implementation rules (each cost a debugging session once)

1. `smartt_reps_set_algo_params` **copies** validated values into the live `params`
   array (framework does NOT auto-copy), with `param_id_base + param_num` bounds-checked
   together.
2. `doca_pcc_dev_user_algo` fetches params/counters pinned to **port 0, slot 0** — never
   the event's port.
3. `doca_pcc_dev_user_set_algo_params` returns **FAIL** for any `algo_slot != 0`.
