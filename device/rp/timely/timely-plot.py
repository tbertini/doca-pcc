#!/usr/bin/env python3
"""
Plot TIMELY PCC trace data from doca_pcc output.

Usage:
    # Pipe live:
    doca_pcc --rp-timely -d mlx5_0 -l 70 2>&1 | tee trace.log
    # Then plot:
    python3 timely-plot.py trace.log --tlow 10000 --thigh 80000

    # Or with prettified input:
    python3 timely-plot.py trace.log --tlow 10000 --thigh 80000 --prettified
"""

import argparse
import re
import sys
import matplotlib.pyplot as plt
import numpy as np


def parse_format12_raw(line):
    """Parse raw format 12 line (hex values)."""
    m = re.search(
        r'format 12.*?'
        r'new_rtt=(?:0x)?([0-9a-fA-Fx]+).*?'
        r'rtt_diff=(?:0x)?(-?[0-9a-fA-Fx]+).*?'
        r'norm_grad=(?:0x)?(-?[0-9a-fA-Fx]+).*?'
        r'branch=(?:0x)?([0-9a-fA-Fx]+).*?'
        r'new_rate=(?:0x)?([0-9a-fA-Fx]+)',
        line
    )
    if not m:
        return None
    return {
        'new_rtt': int(m.group(1), 0),
        'rtt_diff': int(m.group(2), 0),
        'norm_grad': int(m.group(3), 0),
        'branch_raw': int(m.group(4), 0),
        'new_rate': int(m.group(5), 0),
    }


def parse_format12_prettified(line):
    """Parse prettified format 12 line (decimal values, named branches)."""
    m = re.search(
        r'format 12.*?'
        r'new_rtt=(\d+).*?'
        r'rtt_diff=(-?\d+).*?'
        r'norm_grad=(-?\d+).*?'
        r'branch=(\S+).*?'
        r'new_rate=(\d+)',
        line
    )
    if not m:
        return None
    branch_str = m.group(4)
    branch_raw = 0
    if 'T_LOW' in branch_str:
        branch_raw = 0
    elif 'T_HIGH' in branch_str:
        branch_raw = 1
    elif 'GRAD_LE' in branch_str:
        branch_raw = 2
    elif 'GRAD_GT' in branch_str:
        branch_raw = 3
    if 'CLAMP' in branch_str:
        branch_raw |= 0x80
    return {
        'new_rtt': int(m.group(1)),
        'rtt_diff': int(m.group(2)),
        'norm_grad': int(m.group(3)),
        'branch_raw': branch_raw,
        'new_rate': int(m.group(5)),
    }


def parse_format15(line, prettified=False):
    """Parse cadence format 15 line."""
    m = re.search(
        r'format 15.*?'
        r'inter_arrival_ns=(?:0x)?([0-9a-fA-Fx]+).*?'
        r'new_rtt_ns=(?:0x)?([0-9a-fA-Fx]+)',
        line
    )
    if not m:
        return None
    return {
        'inter_arrival': int(m.group(1), 0),
        'new_rtt': int(m.group(2), 0),
    }


def branch_label(raw):
    """Convert branch_raw to label."""
    base = raw & 0x0F
    clamped = bool(raw & 0x80)
    names = {0: 'T_LOW', 1: 'T_HIGH', 2: 'GRAD_LE', 3: 'GRAD_GT'}
    label = names.get(base, f'?{base}')
    if clamped:
        label += '+CLAMP'
    return label


def branch_color(raw):
    """Color per branch type."""
    base = raw & 0x0F
    colors = {0: '#2196F3', 1: '#F44336', 2: '#4CAF50', 3: '#FF9800'}
    return colors.get(base, 'gray')


FXP20_LINE_RATE = 1 << 20  # 1048576 = 100% line rate


def main():
    parser = argparse.ArgumentParser(description='Plot TIMELY PCC traces')
    parser.add_argument('logfile', help='Path to trace log file')
    parser.add_argument('--tlow', type=float, required=True,
                        help='T_LOW threshold in nanoseconds')
    parser.add_argument('--thigh', type=float, required=True,
                        help='T_HIGH threshold in nanoseconds')
    parser.add_argument('--prettified', action='store_true',
                        help='Input was processed by timely-prettify.sh')
    parser.add_argument('--link-gbps', type=float, default=100.0,
                        help='Link speed in Gbps (default: 100)')
    parser.add_argument('--output', '-o', type=str, default=None,
                        help='Save plot to file instead of displaying')
    args = parser.parse_args()

    parse_fn = parse_format12_prettified if args.prettified else parse_format12_raw

    # Parse data
    samples = []
    cadence = []
    with open(args.logfile) as f:
        for line in f:
            if 'format 12' in line:
                s = parse_fn(line)
                if s:
                    samples.append(s)
            elif 'format 15' in line:
                c = parse_format15(line, args.prettified)
                if c:
                    cadence.append(c)

    if not samples:
        print('No format 12 (RTT) events found in log.', file=sys.stderr)
        sys.exit(1)

    n = len(samples)
    idx = np.arange(n)
    rtt = np.array([s['new_rtt'] for s in samples], dtype=float)
    rate_fxp = np.array([s['new_rate'] for s in samples], dtype=float)
    rtt_diff = np.array([s['rtt_diff'] for s in samples], dtype=float)
    norm_grad = np.array([s['norm_grad'] for s in samples], dtype=float)
    branches = [s['branch_raw'] for s in samples]
    colors = [branch_color(b) for b in branches]

    # Convert rate: FXP20 -> Gbps
    rate_gbps = (rate_fxp / FXP20_LINE_RATE) * args.link_gbps

    # Determine subplot count
    has_cadence = len(cadence) > 0
    nplots = 5 if has_cadence else 4

    fig, axes = plt.subplots(nplots, 1, figsize=(14, 3 * nplots), sharex=False)
    fig.suptitle(
        f'TIMELY Trace Analysis  |  T_LOW={args.tlow/1000:.1f} us, '
        f'T_HIGH={args.thigh/1000:.1f} us  |  {n} RTT samples',
        fontsize=13, fontweight='bold'
    )

    # --- Plot 1: RTT over time with thresholds ---
    ax = axes[0]
    ax.scatter(idx, rtt / 1000, c=colors, s=8, alpha=0.7, zorder=2)
    ax.axhline(args.tlow / 1000, color='blue', ls='--', lw=1, label=f'T_LOW={args.tlow/1000:.1f} us')
    ax.axhline(args.thigh / 1000, color='red', ls='--', lw=1, label=f'T_HIGH={args.thigh/1000:.1f} us')
    ax.set_ylabel('RTT (us)')
    ax.set_title('RTT per sample (color = branch taken)')
    ax.legend(loc='upper right', fontsize=8)
    ax.grid(True, alpha=0.3)

    # --- Plot 2: Rate over time ---
    ax = axes[1]
    ax.plot(idx, rate_gbps, color='#333', lw=0.8, alpha=0.6)
    ax.scatter(idx, rate_gbps, c=colors, s=8, alpha=0.7, zorder=2)
    ax.set_ylabel(f'Rate (Gbps, {args.link_gbps:.0f}G link)')
    ax.set_title('Sending rate per sample')
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)

    # --- Plot 3: rtt_diff (smoothed EWMA) ---
    ax = axes[2]
    ax.bar(idx, rtt_diff / 1000, width=1.0, color=colors, alpha=0.7)
    ax.axhline(0, color='black', lw=0.5)
    ax.set_ylabel('rtt_diff (us)')
    ax.set_title('Smoothed RTT difference (EWMA) -- positive = queue growing')
    ax.grid(True, alpha=0.3)

    # --- Plot 4: Branch distribution pie + normalized gradient histogram ---
    ax = axes[3]
    # Left: branch distribution
    from collections import Counter
    branch_counts = Counter(branch_label(b) for b in branches)
    labels_pie = list(branch_counts.keys())
    sizes_pie = list(branch_counts.values())
    color_map = {
        'T_LOW': '#2196F3', 'T_HIGH': '#F44336', 'GRAD_LE': '#4CAF50', 'GRAD_GT': '#FF9800',
        'T_LOW+CLAMP': '#90CAF9', 'T_HIGH+CLAMP': '#EF9A9A',
        'GRAD_LE+CLAMP': '#A5D6A7', 'GRAD_GT+CLAMP': '#FFCC80',
    }
    pie_colors = [color_map.get(l, 'gray') for l in labels_pie]

    ax_left = fig.add_axes(ax.get_position().bounds)
    ax.set_visible(False)
    ax_left.set_position([ax.get_position().x0, ax.get_position().y0,
                          ax.get_position().width * 0.45, ax.get_position().height])
    ax_left.pie(sizes_pie, labels=[f'{l}\n({c})' for l, c in zip(labels_pie, sizes_pie)],
                colors=pie_colors, textprops={'fontsize': 8})
    ax_left.set_title('Branch distribution', fontsize=10)

    ax_right = fig.add_axes([ax.get_position().x0 + ax.get_position().width * 0.55,
                             ax.get_position().y0,
                             ax.get_position().width * 0.45,
                             ax.get_position().height])
    ax_right.hist(norm_grad, bins=range(int(norm_grad.min()) - 1, int(norm_grad.max()) + 3),
                  color='#7E57C2', alpha=0.7, edgecolor='white')
    ax_right.set_xlabel('normalized_gradient (rtt_diff / minRTT)')
    ax_right.set_ylabel('count')
    ax_right.set_title('Gradient distribution', fontsize=10)
    ax_right.grid(True, alpha=0.3)

    # --- Plot 5: Cadence (if available) ---
    if has_cadence:
        ax = axes[4]
        cadence_arr = np.array([c['inter_arrival'] for c in cadence], dtype=float)
        cadence_idx = np.arange(len(cadence_arr))
        ax.scatter(cadence_idx, cadence_arr / 1000, s=10, color='#7E57C2', alpha=0.7)
        ax.set_ylabel('Inter-arrival (us)')
        ax.set_xlabel('RTT event #')
        ax.set_title('Probe cadence (inter-arrival between consecutive RTT events)')
        ax.grid(True, alpha=0.3)

    plt.tight_layout(rect=[0, 0, 1, 0.96])

    if args.output:
        plt.savefig(args.output, dpi=150, bbox_inches='tight')
        print(f'Saved to {args.output}')
    else:
        plt.show()

    # Print summary stats
    print(f'\n--- Summary ({n} RTT samples) ---')
    print(f'  RTT:  min={rtt.min()/1000:.1f} us  median={np.median(rtt)/1000:.1f} us  '
          f'max={rtt.max()/1000:.1f} us  p99={np.percentile(rtt, 99)/1000:.1f} us')
    print(f'  Rate: min={rate_gbps.min():.2f} Gbps  median={np.median(rate_gbps):.2f} Gbps  '
          f'max={rate_gbps.max():.2f} Gbps')
    print(f'  Branches: {dict(branch_counts)}')
    if has_cadence and len(cadence_arr) > 0:
        print(f'  Cadence: min={cadence_arr.min()/1000:.1f} us  '
              f'median={np.median(cadence_arr)/1000:.1f} us  '
              f'max={cadence_arr.max()/1000:.1f} us')


if __name__ == '__main__':
    main()
