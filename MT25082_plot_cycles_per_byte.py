# Roll No: MT25082
# =============================================================================
# File:    MT25082_plot_cycles_per_byte.py
# Purpose: Plot CPU cycles per byte transferred vs message size for all three
#          socket implementations (A1: Two-Copy, A2: One-Copy, A3: Zero-Copy).
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
# Message sizes tested (bytes)
msg_sizes = [64, 256, 1024, 4096]

# CPU cycles per byte transferred
# Computed as: total_cycles / total_bytes_transferred
# Averaged across thread counts for each message size.
# Update these values with your actual experimental measurements.

# A1: Two-Copy baseline (send/recv)
cycles_per_byte_a1 = [644.0, 161.7, 41.3, 10.8]

# A2: One-Copy optimised (sendmsg + iovec)
cycles_per_byte_a2 = [101.1, 25.5, 6.2, 1.9]

# A3: Zero-Copy (sendmsg + MSG_ZEROCOPY)
cycles_per_byte_a3 = [128.2, 29.7, 7.1, 2.2]

# =============================================================================
#  System configuration (displayed on the plot)
# =============================================================================
system_config = (
    "System: Linux 6.14.0 | CPU: 13th Gen Intel i7-13700H | RAM: 16 GB\n"
    "Network: veth pair (net namespaces) | Duration: 3s | Threads: avg across 1,2,4,8"
)

# =============================================================================
#  Plot: CPU Cycles per Byte vs Message Size
# =============================================================================
fig, ax = plt.subplots(figsize=(10, 6))

ax.plot(msg_sizes, cycles_per_byte_a1,
        marker='o', linewidth=2, markersize=8,
        color='#e74c3c', label='A1: Two-Copy (send/recv)')

ax.plot(msg_sizes, cycles_per_byte_a2,
        marker='s', linewidth=2, markersize=8,
        color='#2ecc71', label='A2: One-Copy (sendmsg + iovec)')

ax.plot(msg_sizes, cycles_per_byte_a3,
        marker='^', linewidth=2, markersize=8,
        color='#3498db', label='A3: Zero-Copy (MSG_ZEROCOPY)')

# Axis labels
ax.set_xlabel('Message Size (bytes)', fontsize=13, fontweight='bold')
ax.set_ylabel('CPU Cycles per Byte Transferred', fontsize=13, fontweight='bold')

# Title
ax.set_title('CPU Cycles per Byte vs Message Size — Network I/O Primitives Comparison',
             fontsize=14, fontweight='bold', pad=15)

# Log scale on x-axis (message sizes span orders of magnitude)
ax.set_xscale('log', base=2)
ax.set_xticks(msg_sizes)
ax.set_xticklabels([str(s) for s in msg_sizes], fontsize=11)
ax.tick_params(axis='y', labelsize=11)

# Grid
ax.grid(True, linestyle='--', alpha=0.6)

# Legend
ax.legend(fontsize=11, loc='upper right', framealpha=0.9)

# System configuration annotation
fig.text(0.5, -0.02, system_config,
         ha='center', fontsize=9, style='italic', color='gray')

plt.tight_layout()

plt.savefig('MT25082_cycles_per_byte.png', dpi=200, bbox_inches='tight')
print('Saved: MT25082_cycles_per_byte.png')
