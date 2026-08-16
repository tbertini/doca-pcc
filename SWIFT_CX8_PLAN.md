# Plan: `swift_cx8` — paper-faithful Swift assuming a working credit actuator

Written by Fable 5 for the implementing model (Opus). All file:line anchors were verified
on branch `pre-master` at the time of writing. Read CLAUDE.md first; its rules apply
(nothing compiles on this Mac, commit freely but NEVER push, no Co-Authored-By trailer).

## 1. Goal and framing

Thomas wants a second Swift implementation, `swift_cx8`, that is as close to the paper
(Kumar et al., SIGCOMM 2020) as the platform allows **under the assumption that
`doca_pcc_dev_results_t.credits` / `reload_credits` actually reach the hardware** (they
do not on any DOCA 2.10 chip — proven by disassembly of `libdoca_pcc_dev.a`: the
post-algo glue reads only `rate`+`rtt_req`, plus `probe_type_slot` on CX-8; offsets 12/16
are never loaded). This is therefore a **reference implementation**: it must compile and
run, but on BF3 the credits writes are silently ignored, and because it sets
`rate = MAX_RATE` whenever cwnd ≥ 1 packet, on BF3 it degenerates to an uncontrolled
sender. It is NOT for benchmarking on current hardware. Say this loudly in its README.

External corroboration (checked 2026-08-15): no NVIDIA doc in any DOCA version (including
the CX-8-specific 2.9.0-cx8 archive and 3.4.0) says more about credits/reload_credits than
the two header one-liners; no public GitHub code uses `reload_credits`; and SwCC (Huang et
al., USENIX ATC'25, §3.4) independently states "PCC only allows developers to adjust
per-flow rates and cannot support credit-based or window-based CCAs". The unit and
reload semantics assumptions below therefore cannot be resolved from any public source —
keep them as FIXMEs, and cite SwCC in the README/thesis as third-party confirmation that
the fields are non-functional.

Two paper-faithfulness gains over `device/rp/swift/` (keep everything else identical):
1. The window is actuated as a **window** (credits), not mapped to a rate via BDP.
2. The AIMD controller runs **per ACK event** with the paper's `(ai/cwnd)*num_acked`
   scaling, not once per RTT-probe event.

What stays deviant (signal-side, unavoidable, already documented in the thesis): delay
still comes from RTT probes (responder-reflected t2/t4, sender decomposes), arrives once
per probe round-trip, fabric-only (no endpoint window).

## 2. Locked design decisions

### Identity
- Directory `device/rp/swift_cx8/`, app name `pcc_rp_swift_cx8_app`, symbol prefix
  `swift_cx8_` / `SWIFT_CX8_`, ctxt struct `cc_ctxt_swift_cx8_t`.
- `SWIFT_CX8_ALGO_ID = 0xBFFA` (registry: 0xBFFF template, 0xBFFE dcqcn, 0xBFFD timely,
  0xBFFC swift, 0xBFFB smartt — 0xBFFA is the next free ID). Version 1.0, desc string
  `"Swift-CX8 v1.0 credit-window (Kumar et al. SIGCOMM 2020)"`.
- Trace formats: **reuse 15–19** (Thomas's explicit instruction; only one PCC app runs
  per card, and the host `trace_message_formats[]` table at host/pcc_core.c:39 is shared,
  so do NOT touch the host strings). Format 6 per-event summary reused as-is.
- CLI flag `--rp-swift-cx8`, short name `rp-sw8`. No pcc_params.json work (DOCA argp
  maps JSON keys from long names automatically).
- PPCC contract: **identical param/counter tables** to swift (copy `swift_params.h`,
  rename guards/prefixes, change only the ALGO_ID). `trace_on` stays last. No new params
  in v1.

### Actuation (the core change)
One shared helper in `swift_cx8.c`, called by EVERY handler as the last step (every
handler must fully set `results`):

```c
/* Actuate fcwnd as a credit window (assumed live on CX-8).
 * cwnd >= 1 pkt: credits carry the window, rate limiter wide open (ACK clocking).
 * cwnd <  1 pkt: credits floor at 1, the FXP20 rate implements the paper's
 *                pacing_delay = rtt/cwnd regime (Algorithm 1 lines 29-32). */
static inline void swift_cx8_actuate(uint32_t *param, uint32_t *counter,
                                     cc_ctxt_swift_cx8_t *ccctx,
                                     doca_pcc_dev_results_t *results)
{
    uint32_t cwnd = ccctx->fcwnd;                       /* FXP16 packets */
    uint32_t rate, credits;
    if (cwnd >= SWIFT_CX8_CWND_ONE_FXP16) {
        credits = (cwnd + (1u << 15)) >> 16;            /* round to whole packets */
        rate = DOCA_PCC_DEV_MAX_RATE;                   /* window governs, not the limiter */
    } else {
        credits = 1;                                    /* can't express fractional credits */
        rate = swift_cx8_cwnd_to_rate(param, counter, cwnd,
                                      swift_cx8_pacing_rtt_ns(param, ccctx));
    }
    ccctx->cur_rate = rate;
    results->rate = rate;
    results->credits = credits;
    results->reload_credits = credits;                  /* FIXME: semantics unknown, see below */
}
```

`reload_credits = credits` is an assumption. Put a FIXME on it listing the two rival
readings: (a) an edge-triggered "reload now" command (would be 0/1), (b) the refill
value for a credit bucket. Neither is documented anywhere (fields exist only in
doca_pcc_dev.h:75-76, no helpers, no reference-app usage, no backing field in the BF3
LE structs). Same FIXME style for "credits are integer packets" (could be bytes/MTUs).

`rtt_req` is NOT set by the helper — each handler keeps its existing rtt_req logic
(probe machine unchanged).

### Event mapping (vs. swift.c:784-814 dispatch)
- **new flow** (`cur_rate == 0` sentinel still works; cur_rate is never 0 after init):
  as swift_handle_new_flow (swift.c:389-420), then `swift_cx8_actuate` + `rtt_req = 1`.
  Also zero the new PSN-tracker fields. fmt-15 trace unchanged (its `rate` arg = the
  actuated rate, now typically MAX_RATE).
- **ACK event — becomes THE MAIN HANDLER** (paper Algorithm 1 "On Receiving ACK").
  Signature gains the `counter` array (current swift_handle_ack, swift.c:760-768, doesn't
  take it). Steps:
  1. `retx_cnt = 0` (keep).
  2. `num_acked` from ΔPSN of `doca_pcc_dev_get_roce_ack_first_sn(event)` with
     `doca_pcc_dev_get_ev_attr...num_coalesced` fallback — **copy the
     `smartt_reps_psn_delta` idiom verbatim** (smartt_reps.c:316-347: wrap-safe delta,
     sanity cap `max(2*cwnd_pkts+2, 256)`, cap the ncoal fallback too, PSN_VALID flag).
     Needs ctxt fields `last_ack_psn` + a flags word (see layout below).
  3. `delay = ccctx->last_fabric_rtt`. If 0 (no probe sample yet): skip AIMD entirely,
     actuate current state, return — never regulate on a missing signal.
  4. `target = swift_cx8_flow_scaled_target(param, cwnd, counter)` (unchanged copy).
  5. AI branch (`delay < target`), restoring the paper's per-ACK scaling that swift.c
     deliberately collapsed (see swift.c:592-600 comment — delete that comment logic):
     - cwnd ≥ 1: `cwnd += (uint32_t)min((((uint64_t)ai << 16) / cwnd) * num_acked, sat)`
       (ai/cwnd in FXP16 via `((u64)ai<<16)/cwnd`, times integer num_acked, u64
       intermediate, saturate before narrowing; max_cwnd clamp right after makes the
       saturation bound uncritical).
     - cwnd < 1: `cwnd += ai * num_acked` (u64, saturate).
     - num_acked == 0 → adds 0; that is correct (nothing acked, no increase).
  6. MD branch (`delay >= target`): identical math to swift.c:604-627 (β-scaled cut,
     max_mdf floor, `can_decrease` once-per-RTT guard). The guard period: use
     `last_fabric_rtt` (fall back to `min_rtt` param if 0) — we do not store total RTT.
     Note in a comment that with per-ACK invocation this guard is now load-bearing
     (many ACKs per RTT), unlike the probe-cadence version where it was ~always true.
  7. Clamp cwnd, `t_last_decrease` update if `cwnd <= cwnd_prev` (Algorithm 1 lines
     26-28, as swift.c:629-634), store, actuate. `rtt_req = 0`.
  8. Traces: emit fmt 16 then fmt 17 (same field lists as swift.h:83-84) with
     `np_turnaround` slot carrying **num_acked** instead (host label `np_turn=` is
     cosmetic; document the remapping in the README trace table). Volume guard: ACK
     events are ~5-pkt-coalesced at line rate — even trace_on-gated, per-ACK emission
     would melt the tracer. Emit on every MD / MD_BLOCKED branch, but on AI only when
     `(psn & 63) == 0`.
- **RTT event — demoted to sample refresh + probe machine.** Copy swift_handle_rtt
  (swift.c:484-678) but delete the whole Algorithm-1 block (lines 559-661): keep the
  spurious-event drop, probe_in_flight/abort bookkeeping, t1/t2/t4/t6 extraction, fmt-19
  trace, the unstamped/inconsistent drop path (counter + re-arm), `last_fabric_rtt`
  store, `retx_cnt = 0`, then actuate + `rtt_req = 1`. No fmt 16/17 here anymore, no
  cwnd change.
- **TX event**: unchanged probe abort/retry machine (swift.c:431-466), actuate at the end.
- **NACK event**: unchanged logic (swift.c:697-750) — retx threshold reset / max_mdf cut —
  but actuate via the helper (replaces the swift_cwnd_to_rate call at swift.c:734) and
  keep the always-on fmt-18 trace.
- **CNP / default**: pass-through + actuate.

### Context layout (48 bytes, hard cap — recount before finishing)
Replace swift_ctxt.h's `reserved[3]` tail with:
```c
    uint32_t last_ack_psn;   /* ΔPSN tracker (smartt idiom) */
    uint32_t flags;          /* bit0 = PSN_VALID */
    uint32_t reserved[1];
```
Running total: 4×4 (cur_rate,fcwnd,t_last_decrease,probe_start_ts) + 2×4 (ecwnd,ewma)
+ 4×2 (retx_cnt,probe_in_flight,abort_cnt,tx_since_probe_req) + 4 (last_fabric_rtt)
+ 4 + 4 + 4 = 48. Do not add a `_Static_assert` (dpacc/C11 support unverified); keep the
comment-discipline of the original header.

## 3. Work list (order matters)

1. `cp -r device/rp/swift device/rp/swift_cx8`, rename files:
   `rp_swift_cx8_dev_main.c`, `algo/swift_cx8.{c,h}`, `algo/swift_cx8_ctxt.h`,
   `algo/swift_cx8_params.h`, `algo/swift_cx8_algo_params.h`. Mechanical prefix rename
   throughout (`swift_` → `swift_cx8_`, `SWIFT_` → `SWIFT_CX8_`, include guards too).
   Keep `SWIFT_CX8_DEBUG` as the debug macro name.
2. Apply the semantic changes of §2 to `swift_cx8.c` / `swift_cx8_ctxt.h` /
   `rp_swift_cx8_dev_main.c` (main-file changes: includes, comment header, the fmt-6
   trace block stays as-is). ALGO_ID + desc string in `swift_cx8_params.h` / `swift_cx8.c`.
   Update the big file-top comment in swift_cx8.c to describe the credit-window design
   (mirror §1 framing, FIXME-with-citation style per CLAUDE.md).
3. `build_device_code.sh`: new `elif [ ${PCC_APP_NAME} = "pcc_rp_swift_cx8_app" ]` block
   cloned from the swift one (lines 116-131), env var `SWIFT_CX8_DEBUG`, flag var
   `SWIFT_CX8_DEBUG_FLAG="-DSWIFT_CX8_DEBUG"`; append `${SWIFT_CX8_DEBUG_FLAG}` to the
   APP_FLAGS join at line 189.
4. `meson.build`: clone the swift stanza (lines 82-87) for `pcc_rp_swift_cx8_app`; add
   `pcc_rp_swift_cx8_app` to the executable dependencies list (line 115); clone the
   `swift_params_dump` stanza (lines 150-156) into `swift_cx8_params_dump` pointing at
   `host/swift_cx8_params_dump.c` + `device/rp/swift_cx8/algo`.
5. `host/swift_cx8_params_dump.c`: copy of `host/swift_params_dump.c` with the include
   and any `swift_`-prefixed identifiers switched to `swift_cx8_`.
6. `host/pcc_core.h`: `extern struct doca_pcc_app *pcc_rp_swift_cx8_app;` next to line 115.
7. `host/pcc_core.c`: clone `rp_swift_callback` (line 592) → `rp_swift_cx8_callback`
   selecting `pcc_rp_swift_cx8_app`; clone the argp registration block (lines 974-987)
   with long name `rp-swift-cx8`, short `rp-sw8`, help text
   `"<PCC Reaction Point Swift CX8 (credit window)>"`. Do NOT touch
   `trace_message_formats[]`.
8. `device/rp/swift_cx8/README.md`: write fresh (do not copy swift's). Must contain:
   the credits-are-dead-on-current-silicon warning + "runs uncontrolled on BF3, not for
   benchmarking"; the assumption FIXMEs (credit units, reload_credits semantics); the
   event→Algorithm-1 mapping table; the trace table for 15-19 noting fmt 16's third
   field now prints num_acked under the host's `np_turn=` label and the AI-branch 1/64
   sampling; params contract = swift's with algo_id 0xBFFA.
9. CLAUDE.md: add a row to the branch/registry table: `pre-master (sibling tree)` /
   `Swift-CX8 (credit-window Swift)` / `0xBFFA` / `15–19 (shared with swift)`. One
   sentence in the row or a footnote that trace IDs are shared deliberately.
10. Commits: logical units are fine (device tree; build+host glue; docs) or one commit.
    Message style matches history (`Swift-CX8: ...`). No Co-Authored-By. NEVER push.

## 4. Verification (no compiler on this Mac)

- `grep -rn "swift_[^c]" device/rp/swift_cx8/` and `grep -rn "SWIFT_[^C]" ...` style
  sweeps for missed renames (expect zero hits that aren't `swift_cx8_`/`SWIFT_CX8_`).
- Recount the ctxt struct to exactly 48 bytes, field by field, in the header comment.
- Check every handler path writes `results->rate`, `->credits`, `->reload_credits`,
  `->rtt_req` on every exit (the drop/early-return paths in the RTT and ACK handlers
  are the easy ones to miss).
- Confirm `swift_cx8_params.h` still ends with `TRACE_ON` last and NUM_PARAMS intact.
- Confirm the meson dependencies list and APP_FLAGS join both gained the new entries.
- Remind Thomas at the end: BF3-side `meson setup --wipe` required (device code builds
  at setup, not ninja), and this app is compile-checked only — its credits writes no-op
  on his card.
