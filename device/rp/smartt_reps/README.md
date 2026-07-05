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
| cwnd actuation | `rate` is the **only** actuator (credits unbacked on BF3) | BDP identity, exactly like swift: `rate_fxp20 = clamp((cwnd_fxp16 << 20)/bdp_fxp16, min_rate, MAX)` with `bdp_fxp16 = line_rate_mbps·base_rtt_ns·2^16/(mss_bytes·8000)` |
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

Per **ACK event** (`DOCA_PCC_DEV_EVNT_ROCE_ACK` = 4), with `size` = ΔPSN packets,
`ncoal` = `num_coalesced`, `rtt` = stored fabric_rtt, `ecn` = CNP-hold predicate:

```
acked  += size   (sat16)          # Alg1 line 4
ignored += size  (sat16)          # Alg1 line 5   (bytes_ignored, in pkts)
if ignored < ignore_target:       # Alg1 lines 8-10: post-QuickAdapt blackout (§3.3)
    counter[ACKS_IGNORED]++ ; emit fmt20(branch=BLACKOUT) ; return (rate unchanged)
avg_wtd = α·(ecn?1:0) + (1−α)·avg_wtd          # WTD §3.6.1, once per ACK event
can_decrease = (avg_wtd ≥ wtd_thresh)          # thresh=0 disables WTD gating
adp  = quick_adapt(now)           # Alg2 (below) — always called, may fire
finc = fast_increase(size,ncoal)  # Alg3 (below) — always called, may add k·mtu·ncoal
if adp or finc: goto clamp        # Alg1 lines 15-17 (deviation: we still clamp+actuate)
if      ecn and rtt ≤ trtt and can_decrease:  FD          # §3.2.1 Eq.1
elif    ecn and rtt > trtt and can_decrease:  MD (+FD)    # §3.2.2 Eq.2 ("additionally applies FD")
elif  ! ecn and rtt > trtt:                   FI          # §3.2.3 Eq.3
elif  ! ecn and rtt ≤ trtt:                   MI (+FI)    # §3.2.4 Eq.4 ("followed by FI")
else: (ecn but !can_decrease)  counter[WTD_BLOCKED]++     # no window change
clamp:  cwnd = clamp(cwnd, min_cwnd, fxp_mult(max_cwnd_frac, bdp_fxp16))   # Alg1 line 36 + §3.5
actuate: rate = clamp((cwnd << 20)/bdp, min_rate, DOCA_PCC_DEV_MAX_RATE); store; results
```

The four window updates in FXP16-packet units (`cwnd` FXP16, `size` integer pkts,
`bdp` = bdp_fxp16, `mtu` = 1.0 = 65536; every product via uint64):

```
FD:  cwnd -= ((u64)cwnd · fd / bdp) · size                       # Eq.1  (cwnd/bdp)·fd·size
MD:  dec   = ((u64)(rtt−trtt) << 16) / rtt                        # ratio FXP16, rtt>trtt>0
     dec   = fxp_mult(dec, md) · size                             # FXP16 pkts
     cwnd -= min(dec, size << 16)                                 # Eq.2 cap: at most size
     then apply FD as well                                        # §3.2.2 text
FI:  cwnd += fxp_mult( ((u64)size << 32) / cwnd , fi )            # Eq.3  (size/cwnd)·mtu·fi
MI:  inc   = ((u64)(trtt−rtt) << 16) / rtt                        # ratio FXP16
     inc   = min-with (size<<16) of fxp_mult3(inc, (size<<32)/cwnd, mi)   # Eq.4
     cwnd += inc ; then apply FI as well                          # §3.2.4 text
```

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
        cwnd = sat_u32( (max(acked,1) << 16) · qa_scaling >> 16 )  # max(acked,mtu)·qa_scaling, u64 math
        cwnd = clamp(cwnd, min_cwnd, max_cwnd_frac·bdp)      # clamp BEFORE ignore_target: blackout = actuated window
        ignore_target = cwnd >> 16 (round up, ≥1)            # bytes_to_ignore = unacked ≈ cwnd
        ignored = 0
        counter[QA_FIRED]++ ; emit fmt21
    qa_epoch_start = now; qa_epoch_valid = 1
    acked = 0                                  # epoch accumulator reset (line 12)
return adapted
```

**FastIncrease (Algorithm 3)** — `rtt ≈ brtt` means `rtt ≤ fxp_mult(brtt,
fast_inc_thresh)`:

```
if rtt ≤ brtt·fast_inc_thresh and !ecn:
    count += size (sat16)
    if (count << 16) > cwnd or fi_active:
        cwnd += (u64)fast_inc_k · ncoal        # k MTUs per coalesced ACK arrival
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
    uint32_t probe_start_ts;  /* RTT-probe abort machine (swift start_delay)  [4]  */
    uint16_t acked_pkts;      /* acked in current trtt epoch (saturating)     [2]  */
    uint16_t ignore_pkts;     /* bytes_to_ignore, packets                     [2]  */
    uint16_t ignored_pkts;    /* bytes_ignored, packets (saturating)          [2]  */
    uint16_t avg_wtd_fxp16;   /* WTD EWMA, capped 65535 (≈1.0)                [2]  */
    uint16_t fast_inc_count;  /* FastIncrease byte counter, packets (sat)     [2]  */
    uint8_t  flags;           /* bit0 trigger_qa  bit1 fi_active  bit2 psn_valid
                                 bit3 qa_epoch_valid  bit4 cnp_seen
                                 bit5 rtt_probe_outstanding                   [1]  */
    uint8_t  abort_cnt;       /* probe abort backoff exponent                 [1]  */
    uint8_t  rtt_req_to_rtt_sent; /* TX events between req and SENT flag      [1]  */
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
| 3 | `md` | FXP16 | 131072 | 0 | 1310720 | 2.0 (§3.2.2); §4.2: double it in timeout/NACK mode → sweep 2–4 |
| 4 | `fi` | FXP16 | 16384 | 0 | 655360 | 0.25 (§3.5); γ=bdp/ref_bdp scaling note below |
| 5 | `mi` | FXP16 | 0 | 0 | 1310720 | 0 ⇒ derive brtt/(trtt−brtt) on device (§3.2.4; =2.0 at 1.5×) |
| 6 | `qa_scaling` | FXP16 | 52429 | 1 | 131072 | 0.8 (§3.5) |
| 7 | `fast_inc_k` | FXP16pkt | 131072 | 0 | 655360 | k = 2 MTU per ACK (§3.5) |
| 8 | `fast_inc_thresh` | FXP16 | 72090 | 65536 | 262144 | rtt≈brtt tolerance ×1.1 — FIXME: guess, paper says "close to" |
| 9 | `wtd_alpha` | FXP16 | 8192 | 0 | 65536 | α = 0.125 — FIXME: paper never gives α (§3.6.1) |
| 10 | `wtd_thresh` | FXP16 | 16384 | 0 | 65536 | 0.25 (§3.6.1); 0 disables WTD gating |
| 11 | `ecn_cnp_hold_ns` | ns | 10000 | 0 | 10000000 | FIXME: CNP→ECN assertion window; guess ≳1 trtt |
| 12 | `max_cwnd_frac` | FXP16 | 81920 | 16384 | 655360 | 1.25·BDP cap (§3.5; Alg1 line 36 shows 1.0 — text wins, tunable) |
| 13 | `min_cwnd` | FXP16pkt | 65536 | 66 | 6553600 | 1 MTU floor (§3.5) |
| 14 | `init_cwnd` | FXP16pkt | 0 | 0 | 134217728 | 0 ⇒ BDP (paper: initial value has little effect) |
| 15 | `mss_bytes` | bytes | 4096 | 256 | 9000 | segment size for BDP; FIXME verify RoCE MTU in use |
| 16 | `line_rate_mbps` | Mbps | 100000 | 1000 | 800000 | BDP input |
| 17 | `min_rate` | FXP20 | 64 | 1 | 104858 | rate floor (repo convention, = timely/swift) |
| 18 | `trace_on` | bool | 0 | 0 | 1 | diagnostics only — KEEP LAST |

γ note (§3.5 "Scaling"): the paper multiplies `fi`/`mi` by γ = bdp/reference_bdp
(reference = 100 Gbps × 12 µs). This fabric's BDP is ~⅓ of that (100 G × 3.8 µs), so
paper-faithful seeds would be fi≈0.083, mi≈0.67. The device does **not** auto-scale;
defaults keep the un-scaled §3.5 values and the sweep explores both regimes. `fd`/`md`
are fixed regardless of link speed (paper: always react strongly).

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

## Trace formats 20–24 (gated by `trace_on`, swift-style `*_TRACE_ENABLED`)

Host `pcc_core.c` format strings must match these argument orders exactly:

| fmt | emitted | args (5 × uint64) |
|---|---|---|
| 20 | per ACK decision | `branch`(coded below), `rtt_ns`, `cwnd_fxp16`(after), `rate_fxp20`, `size_pkts` |
| 21 | QuickAdapt fire | `acked_epoch_pkts`, `new_cwnd_fxp16`, `ignore_target_pkts`, `rate_fxp20`, `src`(1=ack,2=nack) |
| 22 | FastIncrease active | `count_pkts`, `cwnd_fxp16`, `rate_fxp20`, `rtt_ns`, `ncoal` |
| 23 | raw ACK signal | `first_sn`, `event_sn`, `delta_pkts`, `ncoal`, `avg_wtd_fxp16` |
| 24 | NACK/loss | `nack_first_sn`, `cwnd_fxp16`(after), `rate_fxp20`, `ignored_pkts`, `trigger_qa_state` |

`branch` (fmt 20): low nibble 0=FD 1=MD 2=FI 3=MI 4=wtd_blocked 5=blackout 6=fastinc
7=qa_adapted; OR-ed high bits: 0x80 rate clamped, 0x40 ecn asserted, 0x20 can_decrease.

Format 6 (shared per-event summary) is also emitted from dev_main under the same gate,
like swift/timely.

## Deviations from the paper (complete list)

1. **REPS omitted** (see top).
2. **p.rtt is the last probe measurement**, not per-ACK: BF3 ACKs carry no timestamps.
   Probe cadence is hardware-determined (measure in Phase 2; if ~ms, the delay signal is
   quasi-static between probes and ECN/QA carry the fast path — the four-case loop
   still behaves correctly, just with a slow delay input).
3. **p.ecn is a CNP-hold predicate**, not a per-packet mark bit; WTD's EWMA steps once
   per ACK event, not per packet. α/thresh/hold are the adaptation levers.
4. **NACK replaces trimming/timeout** as the QuickAdapt trigger; per §4.2 the paper's
   timeout mode suggests doubling `md` (sweep). No retransmit control from PCC (RoCE
   go-back-N does that in hardware); Alg1's `retransmit_packet(p)` is a no-op here.
5. **acked bytes ≈ ΔPSN × MSS** from `first_sn` deltas (coalesce-group first-to-first
   telescopes correctly); `num_coalesced` fallback on anomalies.
6. **`bytes_to_ignore = cwnd`** (paper: `unacked`; not exposed on BF3).
7. **QA/FastIncrease returns still clamp+actuate** (paper's early returns skip line 36's
   clamp; QA/FI values are within sane bounds anyway — safety, no behavioral change).
8. **cwnd floor on decreases**: underflow-guarded to `min_cwnd` (1 MTU) per §3.5; the
   paper's pacer-below-1-MTU option is not implemented (incast degree here is 2).
9. **FastIncrease "per ACK"** scaled by `num_coalesced` (one event may carry many ACKs).

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
- `psn_anomaly` growing ⇒ ΔSN model wrong — check fmt 23 (first_sn vs sn semantics).
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
