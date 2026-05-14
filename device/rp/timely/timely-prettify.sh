#!/usr/bin/env bash
#
# timely-prettify — translate ev_type / branch numbers in doca_pcc TIMELY traces
# into human-readable names, with optional ANSI colors.
#
# Usage:
#   /tmp/build/pcc/doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 | timely-prettify
#   timely-prettify --no-color < log.txt
#   timely-prettify --filter-flow 1414831099 < log.txt
#
# Flags:
#   --no-color           Disable ANSI colors (default: on if stdout is a tty)
#   --filter-flow TAG    Drop format-6 lines whose flow_tag != TAG. Other formats
#                        always pass through (they don't include flow_tag).
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
# Rewrite "<name>=<u32>" to "<name>=<signed>" if u32 looks like a small negative
# (i.e. value >= 2^31). Used to fix the host-side %lu printf of int32_t fields.
function unsign_field(line, field,    re, m_start, m_len, hit, eq, val_s, val_n) {
	re = field "=[0-9]+"
	if (match(line, re)) {
		m_start = RSTART; m_len = RLENGTH
		hit = substr(line, m_start, m_len)
		eq = index(hit, "=")
		val_s = substr(hit, eq+1)
		val_n = val_s + 0
		if (val_n >= 2147483648) {
			return substr(line, 1, m_start-1) field "=" (val_n - 4294967296) substr(line, m_start + m_len)
		}
	}
	return line
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

# Skip "reserved for DCQCN" placeholder format strings (only useful at registration time)
drop_reserved && /^format (7|8|9|10) - reserved/ { next }

# --- Format 6: per-event summary ---------------------------------------------
/^format 6 - pcc event:/ {
	# Parse ev_type and flow_tag for filtering / coloring
	ev_t = ""; ft = "";
	if (match($0, /ev_type=[0-9]+/)) {
		ev_t = substr($0, RSTART+8, RLENGTH-8) + 0
	}
	if (match($0, /flow_tag=[0-9]+/)) {
		ft = substr($0, RSTART+9, RLENGTH-9)
	}

	if (drop_acks && ev[ev_t] == "ACK") next
	if (filter_flow != "" && ft != filter_flow) next

	# Substitute ev_type=N → ev_type=NAME
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
	print col $0 C_RESET
	next
}

# --- Format 12: TIMELY RTT event (Algorithm 1 ran) ---------------------------
/^format 12 - timely rtt:/ {
	# rtt_diff and norm_grad are int32_t on the device side but the host format
	# string registered with DOCA prints them as %lu. So a value like 4294967283
	# is really a small negative (-13). Rewrite any field > 2^31 as signed.
	$0 = unsign_field($0, "rtt_diff")
	$0 = unsign_field($0, "norm_grad")

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
	print col $0 C_RESET
	next
}

# --- Format 11: new flow init ------------------------------------------------
/^format 11 - timely new_flow:/ { print C_BOLD C_CYN $0 C_RESET; next }

# --- Format 14: HAI trigger (rare, important) --------------------------------
/^format 14 - timely hai:/      { print C_BOLD C_YLW $0 C_RESET; next }

# --- Format 0: init banner ---------------------------------------------------
/^format 0 - user init:/        { print C_DIM $0 C_RESET; next }

# --- Format 1: DOCA infra trace ----------------------------------------------
/^format 1 - user algo:/        { print C_DIM $0 C_RESET; next }

# --- "Info:" lines from the host harness -------------------------------------
/^Info:/                        { print C_BOLD $0 C_RESET; next }

# Everything else (errors, warnings, leading garbage like "/  7/") passes through
{ print }
' -
