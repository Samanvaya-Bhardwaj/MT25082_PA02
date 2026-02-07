# Roll No: MT25082
# =============================================================================
# File:    MT25082_plot_latency.py
# Purpose: Plot average latency (µs) vs thread count for all three socket
#          implementations (A1: Two-Copy, A2: One-Copy, A3: Zero-Copy).
#
# Note:    All data values are hardcoded from experimental CSV results.
#          This script does NOT read any CSV files.
#          Running this script will always regenerate the identical plot.
# =============================================================================

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

# =============================================================================
#  Hardcoded experimental data
# =============================================================================
# Thread counts tested
thread_counts = [1, 2, 4, 8]

# Average latency (µs per message) — extracted from MT25082_results.csv
# These are averaged across message sizes for each thread count.
# Update these values with your actual experimental measurements.

# A1: Two-Copy baseline (send/recv)
latency_a1 = [14.56, 7.30, 3.95, 2.84]

# A2: One-Copy optimised (sendmsg + iovec)
latency_a2 = [2.24, 1.11, 0.58, 0.41]

# A3: Zero-Copy (sendmsg + MSG_ZEROCOPY)
latency_a3 = [3.43, 1.72, 1.05, 0.78]

# =============================================================================
#  System configuration (displayed on the plot)
# =============================================================================
system_config = (
    "System: Linux 6.14.0 | CPU: 13th Gen Intel i7-13700H | RAM: 16 GB\n"
    "Network: veth pair (net namespaces) | Duration: 3s | Msg sizes: avg across 64,256,1024,4096"
)

# =============================================================================
#  Plot: Latency vs Thread Count
# =============================================================================
fig, ax = plt.subplots(figsize=(10, 6))

ax.plot(thread_counts, latency_a1,
        marker='o', linewidth=2, markersize=8,
        color='#e74c3c', label='A1: Two-Copy (send/recv)')

ax.plot(thread_counts, latency_a2,
        marker='s', linewidth=2, markersize=8,
        color='#2ecc71', label='A2: One-Copy (sendmsg + iovec)')

ax.plot(thread_counts, latency_a3,
        marker='^', linewidth=2, markersize=8,
        color='#3498db', label='A3: Zero-Copy (MSG_ZEROCOPY)')

# Axis labels
ax.set_xlabel('Thread Count', fontsize=13, fontweight='bold')
ax.set_ylabel('Average Latency (µs / message)', fontsize=13, fontweight='bold')

# Title
ax.set_title('Latency vs Thread Count — Network I/O Primitives Comparison',
             fontsize=14, fontweight='bold', pad=15)

# X-axis ticks at exact thread counts
ax.set_xticks(thread_counts)
ax.set_xticklabels([str(t) for t in thread_counts], fontsize=11)
ax.tick_params(axis='y', labelsize=11)

# Grid
ax.grid(True, linestyle='--', alpha=0.6)

# Legend
ax.legend(fontsize=11, loc='upper left', framealpha=0.9)

# System configuration annotation
fig.text(0.5, -0.02, system_config,
         ha='center', fontsize=9, style='italic', color='gray')

plt.tight_layout()

plt.savefig('MT25082_latency.png', dpi=200, bbox_inches='tight')
print('Saved: MT25082_latency.png')
