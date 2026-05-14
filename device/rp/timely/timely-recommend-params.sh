#!/usr/bin/env bash
#
# timely-recommend-params — measure base RTT and NIC line rate, then print
# recommended TIMELY parameter values for the fabric the script is run on.
#
# Why: paper §6.1 defaults (T_low=50us, T_high=500us, delta=10Mbps, minRTT=25us)
# were tuned for a 10 Gbps fabric with ~20us base RTT. On a different fabric
# (different NIC speed, different topology) you must retune. Paper §4.3:
# "minRTT ... is a fixed value representing the wire propagation delay across
# the datacenter network, which is known ahead of time" — i.e. measure it.
#
# Usage:
#   timely-recommend-params.sh --receiver HOST [options]
#
# Required:
#   --receiver HOST       Receiver hostname or IP reachable over the RoCE fabric
#                         (the same host you would pass to ib_write_lat / ib_write_bw)
#
# Optional:
#   --iface IFACE         Local Ethernet netdev for line-rate query     (default: data1)
#                         Tip: ls /sys/class/infiniband/<ib>/device/net/ to find it
#   --ib-device IBDEV     Local IB device for ib_write_lat / ib_write_bw (default: mlx5_0)
#   --samples N           Number of ib_write_lat iterations              (default: 1000)
#   --user USER           SSH user for receiver-side server processes    (default: $USER)
#   --no-ib-bw            Skip the ib_write_bw connectivity check (RTT probe still runs)
#   -h, --help            Show this help.
#
# Probe details:
#   - RTT is measured with ib_write_lat (NIC-level, over the actual RoCE path).
#     Requires an `ib_write_lat -d <ibdev>` server running on the receiver.
#   - Line rate is read from /sys/class/net/<iface>/speed (Ethernet netdev,
#     not the IB device name — they differ). Use `ls /sys/class/infiniband/
#     <ib>/device/net/` to find the right netdev name for your card.
#   - Optional ib_write_bw probe just confirms end-to-end fabric works.
#

set -u

RECEIVER=""
IFACE="data1"
IB_DEVICE="mlx5_0"
SAMPLES=1000
SSH_USER="${USER}"
SKIP_IB_BW=0

while [ $# -gt 0 ]; do
	case "$1" in
		--receiver)   RECEIVER="$2"; shift ;;
		--iface)      IFACE="$2"; shift ;;
		--ib-device)  IB_DEVICE="$2"; shift ;;
		--samples)    SAMPLES="$2"; shift ;;
		--user)       SSH_USER="$2"; shift ;;
		--no-ib-bw)   SKIP_IB_BW=1 ;;
		-h|--help)
			# Print the comment header (skip shebang line 1, stop at first code line).
			awk 'NR==1 { next } /^[^#[:space:]]/ { exit } { print }' "$0" \
				| sed 's/^# \{0,1\}//;s/^#$//'
			exit 0 ;;
		*) echo "unknown flag: $1" >&2; exit 2 ;;
	esac
	shift
done

if [ -z "$RECEIVER" ]; then
	echo "error: --receiver is required" >&2
	echo "see --help" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# Probe 1: NIC-level base RTT via ib_write_lat.
# ib_write_lat reports min/median/avg/max latency in microseconds over the
# actual RoCE path. We use the minimum as our base RTT (wire propagation
# delay), not the average — average includes scheduler jitter.
# Requires `ib_write_lat -d $IB_DEVICE` running on the receiver.
# ---------------------------------------------------------------------------
echo "=== Fabric measurements ==="
echo "Receiver:        $RECEIVER"
echo "Local iface:     $IFACE  (Ethernet netdev for line rate)"
echo "IB device:       $IB_DEVICE  (used for ib_write_lat probe)"
echo "Iterations:      $SAMPLES"
echo

ib_min_us=""
ib_avg_us=""
ib_max_us=""

if ! command -v ib_write_lat >/dev/null 2>&1; then
	echo "Base RTT:        ib_write_lat not installed; cannot measure"
else
	# -n samples, -s 64 (small payload to minimize serialization), 10s overall timeout.
	# -F suppresses CPU-frequency warnings on tuned hosts.
	if timeout 15 ib_write_lat -d "$IB_DEVICE" -n "$SAMPLES" -s 64 -F "$RECEIVER" \
			>/tmp/timely-recommend-ib-lat.log 2>&1; then
		# Output line of interest looks like:
		#  #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]    t_avg[usec]    t_stdev[usec]   99% percentile[usec]   99.9% percentile[usec]
		#  64     1000           1.79           38.18        1.95               2.01           0.91            5.50                   38.18
		row=$(awk '/^[ ]*64[ ]+/ { print; exit }' /tmp/timely-recommend-ib-lat.log)
		if [ -n "$row" ]; then
			ib_min_us=$(echo "$row" | awk '{print $3}')
			ib_max_us=$(echo "$row" | awk '{print $4}')
			ib_avg_us=$(echo "$row" | awk '{print $6}')
			# ib_write_lat reports half-RTT (one-way latency for write completion).
			# Round-trip ≈ 2 × t_min. Display both so the user can sanity-check.
			rtt_min_us=$(awk -v v="$ib_min_us" 'BEGIN { printf "%.2f", v*2 }')
			echo "ib_write_lat one-way: min ${ib_min_us}us, avg ${ib_avg_us}us, max ${ib_max_us}us"
			echo "Base RTT (2× min):    ${rtt_min_us} us"
		else
			echo "Base RTT:        ib_write_lat ran but couldn't parse — see /tmp/timely-recommend-ib-lat.log"
		fi
	else
		echo "Base RTT:        ib_write_lat FAILED — no server on receiver?"
		echo "                 To enable: ssh $SSH_USER@$RECEIVER ib_write_lat -d $IB_DEVICE -n $SAMPLES -s 64 -F"
		echo "                 (run on receiver in another terminal, then re-run this script)"
		echo "                 Last lines of probe output:"
		tail -5 /tmp/timely-recommend-ib-lat.log 2>/dev/null | sed 's/^/                   /'
	fi
fi

# ---------------------------------------------------------------------------
# Probe 2: NIC line rate via sysfs (fall back to ethtool).
# /sys/class/net/<iface>/speed reports Mb/s as plain integer (-1 if link down).
# ---------------------------------------------------------------------------
line_rate_mbps=""
sysfs_speed="/sys/class/net/$IFACE/speed"
if [ -r "$sysfs_speed" ]; then
	v=$(cat "$sysfs_speed" 2>/dev/null || echo "")
	if [ -n "$v" ] && [ "$v" -gt 0 ] 2>/dev/null; then
		line_rate_mbps="$v"
	fi
fi
if [ -z "$line_rate_mbps" ] && command -v ethtool >/dev/null 2>&1; then
	v=$(ethtool "$IFACE" 2>/dev/null | awk '/Speed:/ { gsub(/[^0-9]/,"",$2); print $2 }')
	if [ -n "$v" ] && [ "$v" -gt 0 ] 2>/dev/null; then
		line_rate_mbps="$v"
	fi
fi

if [ -n "$line_rate_mbps" ]; then
	line_rate_gbps=$(awk -v v="$line_rate_mbps" 'BEGIN { printf "%g", v/1000 }')
	echo "Line rate:       ${line_rate_gbps} Gb/s  ($line_rate_mbps Mb/s)"
else
	echo "Line rate:       UNKNOWN (sysfs $sysfs_speed missing/zero, ethtool failed)"
	echo "                 Falling back to assumed 100 Gb/s for the recommendation"
	line_rate_mbps=100000
fi

# ---------------------------------------------------------------------------
# Probe 3 (optional): end-to-end fabric sanity via ib_write_bw.
# Just a 1-second probe to confirm the path works at expected bandwidth.
# Requires the receiver to already be running an `ib_write_bw` server.
# Skip with --no-ib-bw. Note: the RTT probe (ib_write_lat) above is the
# load-bearing measurement; this one is purely informational.
# ---------------------------------------------------------------------------
ib_ok=""
if [ "$SKIP_IB_BW" = "1" ]; then
	echo "IB bandwidth:    skipped (--no-ib-bw)"
elif ! command -v ib_write_bw >/dev/null 2>&1; then
	echo "IB bandwidth:    skipped (ib_write_bw not installed)"
else
	if timeout 5 ib_write_bw -d "$IB_DEVICE" "$RECEIVER" -D 1 -s 65536 --report_gbits >/tmp/timely-recommend-ib-bw.log 2>&1; then
		bw=$(awk '/^[ ]*65536[ ]+/ { print $4 }' /tmp/timely-recommend-ib-bw.log | tail -1)
		if [ -n "$bw" ]; then
			echo "IB bandwidth:    ${bw} Gb/s (ib_write_bw 1s probe)"
			ib_ok=1
		else
			echo "IB bandwidth:    ran but couldn't parse — see /tmp/timely-recommend-ib-bw.log"
		fi
	else
		echo "IB bandwidth:    FAILED — no ib_write_bw server on receiver?"
		echo "                 To enable: ssh $SSH_USER@$RECEIVER ib_write_bw -d $IB_DEVICE -D 5 -s 65536"
	fi
fi
echo

# ---------------------------------------------------------------------------
# Derive recommended parameters
# ---------------------------------------------------------------------------

if [ -z "$ib_min_us" ]; then
	echo "ERROR: cannot recommend parameters without a base RTT measurement." >&2
	echo "Start ib_write_lat on the receiver and rerun:" >&2
	echo "  ssh $SSH_USER@$RECEIVER ib_write_lat -d $IB_DEVICE -n $SAMPLES -s 64 -F" >&2
	exit 3
fi

# MIN_RTT_NS = 2 × ib_write_lat min (us → ns), clamped to [500, 100000].
# ib_write_lat reports one-way write-completion latency; round-trip ≈ 2×.
min_rtt_ns=$(awk -v v="$ib_min_us" 'BEGIN {
	r = v * 2000   # us → ns and ×2 for round-trip
	if (r < 500) r = 500
	if (r > 100000) r = 100000
	printf "%d", r + 0.5
}')

# Sanity warning band — datacenter wire delay is typically 1-50us.
warn=""
if [ "$(awk -v r="$min_rtt_ns" 'BEGIN { print (r < 1000) }')" = "1" ]; then
	warn="WARNING: measured RTT < 1us is suspicious (loopback? local probe?)"
fi
if [ "$(awk -v r="$min_rtt_ns" 'BEGIN { print (r > 50000) }')" = "1" ]; then
	warn="WARNING: measured RTT > 50us is unusual for a datacenter fabric — verify the link is healthy"
fi

# T_LOW and T_HIGH in ns
t_low_ns=$((min_rtt_ns * 2))
t_high_ns=$((min_rtt_ns * 15))

# DELTA_FXP20: paper used delta = 0.001 * line_rate (10Mbps on 10Gbps).
# Apply same ratio to our line rate; convert to FXP20 fraction.
#   delta_fraction = 0.001
#   delta_fxp20 = round(0.001 * (1<<20)) = round(1048.576) = 1049 (or 1048)
# This is *line-rate-relative*, so it's actually independent of line_rate_mbps
# given the 0.001 ratio rule. We compute it via the formula anyway so the
# ratio is auditable.
delta_fxp20=$(awk -v line="$line_rate_mbps" 'BEGIN {
	delta_mbps = line * 0.001            # paper ratio
	frac = delta_mbps / line              # always 0.001
	fxp = frac * 1048576                  # << 20
	printf "%d", fxp + 0.5
}')

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
echo "=== Recommended TIMELY parameters ==="
printf "%-22s %-15s %-15s %s\n" "PARAM" "paper default" "current" "recommended"
printf "%-22s %-15s %-15s %s\n" "TIMELY_MIN_RTT_NS" "n/a (you set)" "25000" "$min_rtt_ns"
printf "%-22s %-15s %-15s %s\n" "TIMELY_T_LOW_NS" "50000" "50000" "$t_low_ns"
printf "%-22s %-15s %-15s %s\n" "TIMELY_T_HIGH_NS" "500000" "500000" "$t_high_ns"
printf "%-22s %-15s %-15s %s\n" "TIMELY_DELTA_FXP20" "105 (10Mbps@10G)" "105" "$delta_fxp20"
echo
[ -n "$warn" ] && echo "$warn" && echo

cat <<EOF
Rationale:
  MIN_RTT_NS  = 2 × ib_write_lat t_min (paper §4.3: wire propagation delay)
  T_LOW_NS    = 2  × MIN_RTT_NS   (paper §4.3: filter burst-induced RTT spikes)
  T_HIGH_NS   = 15 × MIN_RTT_NS   (paper §4.3: tolerable end-to-end queue delay budget)
  DELTA_FXP20 = round(0.001 × (1<<20))  (paper §6.1 ratio: 10Mbps on 10Gbps line)

=== Copy-paste into device/rp/timely/algo/timely_algo_params.h ===
#define TIMELY_MIN_RTT_NS    ($min_rtt_ns)
#define TIMELY_T_LOW_NS      ($t_low_ns)
#define TIMELY_T_HIGH_NS     ($t_high_ns)
#define TIMELY_DELTA_FXP20   ($delta_fxp20)

After editing, rebuild on the server:
  TIMELY_DEBUG=0 meson setup --wipe /tmp/build -Denable_all_applications=false -Denable_pcc=true
  ninja -C /tmp/build
EOF
