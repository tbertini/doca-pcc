#!/usr/bin/env python3
"""Pretty-print PPCC mlxreg output: hex->decimal, plus a parenthetical hint
on each field's meaning. Annotations are TIMELY-aware: text[i] cells are
labelled with the TIMELY param/counter name based on the cmd_type.

NOTE: TIMELY_PARAMS / TIMELY_COUNTERS below mirror the contract defined in
device/rp/timely/algo/timely_params.h. If that header changes (params added,
reordered, renamed), update these lists to match - they are NOT auto-derived.

Usage (one-time, in your shell):

    mlxreg-pretty() { sudo mlxreg "$@" | python3 ~/mlxreg-pretty.py; }

Example invocation (get per-param info for param 0 on slot 0):

    mlxreg-pretty -d /dev/mst/mt41692_pciconf0 --reg_name PPCC -y --get \\
        --indexes "lp_msb=0,pnat=0,local_port=1,target_app=0,algo_slot=0,algo_param_index=0" \\
        --op "cmd_type=0x5"
"""
import sys, re, struct

TIMELY_PARAMS = ["t_low_ns", "t_high_ns", "delta_fxp20", "beta_fxp16",
                 "alpha_fxp16", "min_rtt_ns", "hai_threshold",
                 "min_rate_fxp20", "init_rate_fxp20"]
TIMELY_COUNTERS = ["events", "md_gradient", "md_thigh", "ai", "hai",
                   "rtt_lt_tlow", "rtt_gt_thigh", "rate_clamped_min"]
CMD_OPS = {
    0x0: "get_algo_info",       0x1: "enable",
    0x2: "disable",              0x3: "get_enable_status",
    0x4: "get_num_params",       0x5: "get_param_info",
    0x8: "set_param",            0xA: "bulk_get_params",
    0xB: "bulk_set_params",      0xC: "bulk_get_counters",
    0xD: "bulk_get_clear_counters", 0xE: "get_num_counters",
}
TEXT_HEADER = {
    0x0: "# text[]: algo info blob (vendor-specific)",
    0x5: "# text[]: param name/description for algo_param_index (ASCII, 4 chars/cell, big-endian; text_length = string bytes)",
    0xA: "# text[]: bulk param values, one uint32 per param starting at algo_param_index",
    0xB: "# text[]: bulk param values to set, one uint32 per param starting at algo_param_index",
    0xC: "# text[]: bulk counter values, one uint32 per counter starting at algo_param_index",
    0xD: "# text[]: bulk counter values (and clear after read), one uint32 per counter",
}
FIELD_HINT = {
    "cmd_type":          "operation",
    "algo_slot":         "slot 0..15",
    "algo_param_index":  "param idx (for 0x5/0x8) or start idx (for 0xA/0xB)",
    "target_app":        "subapp selector",
    "local_port":        "NIC port (1-based)",
    "pnat":              "port type (0 = local NIC)",
    "lp_msb":            "local_port MSB",
    "value":             "0x4/0xE: count; 0x8: value to set",
    "param_value1":      "0x5: param default",
    "param_value2":      "0x5: param min",
    "param_value3":      "0x5: param max",
    "sl_bitmask":        "service-level bitmask (0x1: enable)",
    "counter_en":        "counters enabled flag (0x1: enable)",
    "trace_en":          "tracing enabled flag (0x1: enable)",
    "sl_bitmask_support":"firmware supports SL bitmask",
    "text_length":       "bulk data length in bytes",
    "prm":               "param mode",
}

def ascii4_be(n):
    """Decode a uint32 as 4 big-endian bytes -> ASCII string; non-printables shown as '.'"""
    try:
        b = struct.pack(">I", n & 0xFFFFFFFF)
        return "".join(chr(c) if 32 <= c < 127 else "." for c in b)
    except Exception:
        return None

lines = list(sys.stdin)
cmd = None
for l in lines:
    m = re.match(r"\s*cmd_type\s*\|\s*(0x[0-9a-fA-F]+|\d+)", l)
    if m:
        cmd = int(m.group(1), 0); break

text_header_printed = False
for l in lines:
    l = re.sub(r"0x[0-9a-fA-F]+", lambda m: str(int(m.group(), 16)), l).rstrip()
    m = re.match(r"^(\S+(?:\[\d+\])?)\s*\|\s*(.*)$", l)
    if not m:
        print(l); continue
    field, data = m.groups()
    hint = None
    tm = re.match(r"text\[(\d+)\]", field)
    if tm:
        if not text_header_printed:
            print(TEXT_HEADER.get(cmd, "# text[]: bulk payload (cmd-type-specific layout)"))
            text_header_printed = True
        i = int(tm.group(1))
        if cmd in (0xA, 0xB) and i < len(TIMELY_PARAMS):
            hint = f"TIMELY {TIMELY_PARAMS[i]}"
        elif cmd in (0xC, 0xD) and i < len(TIMELY_COUNTERS):
            hint = f"TIMELY {TIMELY_COUNTERS[i]}"
        elif cmd == 0x5:
            try:
                s = ascii4_be(int(data))
                if s and any(c != "." and c != "\x00" for c in s):
                    hint = f"ascii: {s!r}"
            except ValueError: pass
    elif field in FIELD_HINT:
        hint = FIELD_HINT[field]
        if field == "cmd_type":
            try:
                op = CMD_OPS.get(int(data))
                if op: hint = f"{hint} = {op}"
            except ValueError: pass
    if hint:
        print(f"{field:<22}| {data}\t\t({hint})")
    else:
        print(l)
