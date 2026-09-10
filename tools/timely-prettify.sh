#!/usr/bin/env bash
#
# timely-prettify - translate doca_pcc TIMELY trace fields (tsflow, ev_type,
# branch) into human-readable names, with optional ANSI colors.
#
# Per-event formats 6, 11, 12 and 14 lead with tsflow=0x<hex>, a packed 64-bit
# value: bits[63:32] = device timestamp in ns (wraps every ~4.295 s),
# bits[31:0] = flow_tag. The script rewrites it to "ts=<ns> flow=<tag>" and
# filters those formats on the recovered flow tag. Lines carrying no tsflow
# still pass: format 6 falls back to its standalone flow_tag= field, and
# formats 11, 12 and 14 are not filtered.
#
# Usage:
#   /tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 | timely-prettify
#   timely-prettify --no-color < log.txt
#   timely-prettify --filter-flow 1414831099 < log.txt
#   # Capture with a wall-clock prefix ("<unix_epoch.frac> " prepended to each
#   # line); timely-prettify detects and preserves it ahead of the usual fields:
#   /tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 \
#       | perl -MTime::HiRes=time -pe 'BEGIN{$|=1} $_ = join(chr(32), time, $_)' \
#       | timely-prettify
#
# Flags:
#   --no-color           Disable ANSI colors (default: on if stdout is a tty)
#   --filter-flow TAG    Drop lines (formats 6, 11, 12, 14) whose flow tag !=
#                        TAG (decimal). TAG is matched against tsflow's low 32
#                        bits, or the flow_tag= field on format-6 lines that
#                        carry no tsflow. Formats with no flow concept always
#                        pass through.
#   --drop-acks          Drop format-6 lines with ev_type=ACK (most common noise).
#   --drop-reserved      Drop "format N - reserved" placeholder lines.
#   -h, --help           Show this help.
#

set -u

USE_COLOR=auto
FILTER_FLOW=""
DROP_ACKS=0
DROP_RESERVED=0

while [ $# -gt 0 ]; do
	case "$1" in
		--no-color)         USE_COLOR=no ;;
		--color)            USE_COLOR=yes ;;
		--filter-flow)      FILTER_FLOW="$2"; shift ;;
		--drop-acks)        DROP_ACKS=1 ;;
		--drop-reserved)    DROP_RESERVED=1 ;;
		-h|--help)
			sed -n '/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,1\}//'
			exit 0 ;;
		*) echo "unknown flag: $1" >&2; exit 2 ;;
	esac
	shift
done

if [ "$USE_COLOR" = "auto" ]; then
	if [ -t 1 ]; then USE_COLOR=yes; else USE_COLOR=no; fi
fi

awk -v use_color="$USE_COLOR" \
    -v filter_flow="$FILTER_FLOW" \
    -v drop_acks="$DROP_ACKS" \
    -v drop_reserved="$DROP_RESERVED" '
# Portable hex->decimal for one <=8-hex-digit (<=32-bit) chunk. Deliberately
# NOT using strtonum(): it is a gawk-only extension, absent from mawk (the
# default /usr/bin/awk provider on stock Ubuntu, which is what the BlueField
# hosts run) and from the macOS/BSD one-true-awk used for local dev/test on
# this Mac - and the rest of this script already sticks to portable POSIX
# constructs (2-arg match/RSTART/RLENGTH, no gensub/asort/etc.) on purpose, so
# it runs unmodified on whatever awk a given host resolves "awk" to. Splitting
# tsflow into two 32-bit halves before converting (see split_tsflow) keeps
# every value well under 2^53, so plain awk double-precision arithmetic never
# loses precision.
function hex2dec(hexstr,    i, c, n, d) {
	n = 0
	for (i = 1; i <= length(hexstr); i++) {
		c = tolower(substr(hexstr, i, 1))
		if (c ~ /[0-9]/)   d = c + 0
		else if (c == "a") d = 10
		else if (c == "b") d = 11
		else if (c == "c") d = 12
		else if (c == "d") d = 13
		else if (c == "e") d = 14
		else if (c == "f") d = 15
		else               d = 0   # defensive: ignore unexpected chars
		n = n * 16 + d
	}
	return n
}

# Split a tsflow hex string (no "0x" prefix, variable width, %lx is not
# zero-padded) into globals g_ts / g_flow, each a 32-bit decimal. Left-pad to
# 16 hex digits first so the split always lands on the nibble boundary
# between the two 32-bit halves, regardless of how many leading zeros %lx
# elided.
function split_tsflow(hexstr,    padded) {
	padded = hexstr
	while (length(padded) < 16) padded = "0" padded
	if (length(padded) > 16) padded = substr(padded, length(padded) - 15)
	g_ts   = hex2dec(substr(padded, 1, 8))
	g_flow = hex2dec(substr(padded, 9, 8))
}

BEGIN {
	# ev_type lookup (from include/doca_pcc_dev.h)
	ev[0] = "NULL"; ev[1] = "FW"; ev[2] = "CNP"; ev[3] = "TX";
	ev[4] = "ACK";  ev[5] = "NACK"; ev[6] = "RTT";

	# branch lookup (from device/rp/timely/algo/timely.c)
	br["0x0"] = "T_LOW";   br["0x1"] = "T_HIGH";
	br["0x2"] = "GRAD_LE"; br["0x3"] = "GRAD_GT";
	br["0x80"] = "T_LOW+CLAMP";   br["0x81"] = "T_HIGH+CLAMP";
	br["0x82"] = "GRAD_LE+CLAMP"; br["0x83"] = "GRAD_GT+CLAMP";

	if (use_color == "yes") {
		C_RESET="\033[0m"; C_BOLD="\033[1m"; C_DIM="\033[2m";
		C_RED="\033[31m"; C_GRN="\033[32m"; C_YLW="\033[33m";
		C_BLU="\033[34m"; C_MAG="\033[35m"; C_CYN="\033[36m";
	}
}

# --- Normalize every line up front -------------------------------------------
# 1. Peel off an optional leading wall-clock float ("<epoch.frac> ", from the
#    perl-prefix launcher documented in the header) so the "^format N - ..."
#    anchors below still match; re-attach it verbatim at print time via
#    wallprefix.
# 2. Rewrite tsflow=0x<hex> -> "ts=<ns> flow=<tag>" and stash the flow in
#    cur_flow/have_tsflow for the format blocks below to filter on. Lines
#    without tsflow (old-style logs, or formats that never had one) leave
#    have_tsflow=0 and cur_flow="".
{
	wallprefix = ""
	if (match($0, /^[0-9]+\.[0-9]+ /)) {
		wallprefix = substr($0, RSTART, RLENGTH)
		$0 = substr($0, RSTART + RLENGTH)
	}

	have_tsflow = 0; cur_flow = ""
	if (match($0, /tsflow=0x[0-9a-fA-F]+/)) {
		hexpart = substr($0, RSTART + 9, RLENGTH - 9)   # strip "tsflow=0x"
		split_tsflow(hexpart)
		have_tsflow = 1
		cur_flow = g_flow
		sub(/tsflow=0x[0-9a-fA-F]+/, "ts=" g_ts " flow=" g_flow)
	}
}

# Skip "reserved for DCQCN" placeholder format strings (only useful at registration time)
drop_reserved && /^format (7|8|9|10) - reserved/ { next }

# --- Format 6: per-event summary ---------------------------------------------
# New layout: tsflow=0x%lx ev_type=%lu port=%lu algo_slot=%lu rate=%lu
# Old layout: ev_type=%lu flow_tag=%lu port=%lu algo_slot=%lu rate=%lu
# (the position of the ev_type field relative to its neighbors changed
# between the two layouts, but that has never mattered here since it is found
# by regex, not by field number.)
/^format 6 - pcc event:/ {
	ev_t = ""
	if (match($0, /ev_type=[0-9]+/)) {
		ev_t = substr($0, RSTART+8, RLENGTH-8) + 0
	}

	# Prefer the flow tag recovered from tsflow; fall back to the old
	# standalone flow_tag= field for pre-normalization logs.
	ft = cur_flow
	if (!have_tsflow && match($0, /flow_tag=[0-9]+/)) {
		ft = substr($0, RSTART+9, RLENGTH-9)
	}

	if (drop_acks && ev[ev_t] == "ACK") next
	if (filter_flow != "" && ft != filter_flow) next

	# Substitute ev_type=N -> ev_type=NAME
	if (ev_t in ev) {
		sub(/ev_type=[0-9]+/, "ev_type=" ev[ev_t])
	}

	# Color by event class
	col = ""
	if      (ev[ev_t] == "RTT")  col = C_GRN
	else if (ev[ev_t] == "NACK") col = C_RED
	else if (ev[ev_t] == "CNP")  col = C_MAG
	else if (ev[ev_t] == "TX")   col = C_DIM
	else if (ev[ev_t] == "ACK")  col = C_DIM
	print wallprefix col $0 C_RESET
	next
}

# --- Format 12: TIMELY RTT event (Algorithm 1 ran) ---------------------------
# Layout: tsflow=0x%lx new_rtt=%lu norm_grad=%ld branch=0x%lx new_rate=%lu
# norm_grad arrives sign-extended (%ld) from the device; no fixup is needed here.
/^format 12 - timely rtt:/ {
	if (have_tsflow && filter_flow != "" && cur_flow != filter_flow) next

	# Replace branch=0x?? with the named version
	if (match($0, /branch=0x[0-9a-fA-F]+/)) {
		raw = substr($0, RSTART+7, RLENGTH-7)  # the "0x..." part
		if (raw in br) sub(/branch=0x[0-9a-fA-F]+/, "branch=" br[raw])
	}

	# Color: red if a decrease branch (T_HIGH or GRAD_GT), green if increase
	# (T_LOW or GRAD_LE), yellow overlay if clamped.
	col = C_GRN
	if ($0 ~ /branch=(T_HIGH|GRAD_GT)/) col = C_RED
	if ($0 ~ /\+CLAMP/) col = C_YLW
	print wallprefix col $0 C_RESET
	next
}

# --- Format 11: new flow init ------------------------------------------------
# New layout: tsflow=0x%lx rate=%lu (old layout carried no flow tag at all:
# rate= ts= arg3= arg4= arg5=)
/^format 11 - timely new_flow:/ {
	if (have_tsflow && filter_flow != "" && cur_flow != filter_flow) next
	print wallprefix C_BOLD C_CYN $0 C_RESET
	next
}

# --- Format 14: HAI trigger (rare, important) --------------------------------
# New layout: tsflow=0x%lx neg_count=%lu N=%lu delta=%lu new_rate=%lu (old
# layout: neg_count= N= new_rate= delta= arg5=, no flow tag)
/^format 14 - timely hai:/ {
	if (have_tsflow && filter_flow != "" && cur_flow != filter_flow) next
	print wallprefix C_BOLD C_YLW $0 C_RESET
	next
}

# --- Format 0: init banner ---------------------------------------------------
/^format 0 - user init:/        { print wallprefix C_DIM $0 C_RESET; next }

# --- Format 1: DOCA infra trace ----------------------------------------------
/^format 1 - user algo:/        { print wallprefix C_DIM $0 C_RESET; next }

# --- "Info:" lines from the host harness -------------------------------------
/^Info:/                        { print wallprefix C_BOLD $0 C_RESET; next }

# Everything else (errors, warnings, leading garbage like "/  7/") passes through
{ print wallprefix $0 }
' -
