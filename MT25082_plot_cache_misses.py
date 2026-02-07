# Roll No: MT25082
# =============================================================================
# File:    MT25082_plot_cache_misses.py
# Purpose: Plot cache misses (L1 and LLC) vs message size for all three
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

# L1 data cache misses — extracted from MT25082_results.csv
# Averaged across thread counts for each message size.
# Update these values with your actual experimental measurements.

# A1: Two-Copy baseline (send/recv)
l1_misses_a1 = [291968304, 309217729, 307166401, 347778830]

# A2: One-Copy optimised (sendmsg + iovec)
l1_misses_a2 = [286537273, 314324731, 346897605, 513598431]

# A3: Zero-Copy (sendmsg + MSG_ZEROCOPY)
l1_misses_a3 = [295035969, 294291095, 289806198, 454019805]

# LLC (Last-Level Cache) load misses
# A1: Two-Copy baseline
llc_misses_a1 = [26389, 18359, 19765, 24503]

# A2: One-Copy optimised
llc_misses_a2 = [20707, 25050, 78139, 1652451]

# A3: Zero-Copy
llc_misses_a3 = [8207, 9827, 6801, 12790]

# =============================================================================
#  System configuration (displayed on the plot)
# =============================================================================
system_config = (
    "System: Linux 6.14.0 | CPU: 13th Gen Intel i7-13700H | RAM: 16 GB\n"
    "Network: veth pair (net namespaces) | Duration: 3s | Threads: avg across 1,2,4,8"
)

# =============================================================================
#  Plot: Cache Misses vs Message Size (two subplots — L1 and LLC)
# =============================================================================
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

# ---- Left subplot: L1 Data Cache Misses -----------------------------------
ax1.plot(msg_sizes, l1_misses_a1,
         marker='o', linewidth=2, markersize=8,
         color='#e74c3c', label='A1: Two-Copy (send/recv)')

ax1.plot(msg_sizes, l1_misses_a2,
         marker='s', linewidth=2, markersize=8,
         color='#2ecc71', label='A2: One-Copy (sendmsg + iovec)')

ax1.plot(msg_sizes, l1_misses_a3,
         marker='^', linewidth=2, markersize=8,
         color='#3498db', label='A3: Zero-Copy (MSG_ZEROCOPY)')

ax1.set_xlabel('Message Size (bytes)', fontsize=12, fontweight='bold')
ax1.set_ylabel('L1 Data Cache Misses', fontsize=12, fontweight='bold')
ax1.set_title('L1 Cache Misses vs Message Size', fontsize=13, fontweight='bold')
ax1.set_xscale('log', base=2)
ax1.set_xticks(msg_sizes)
ax1.set_xticklabels([str(s) for s in msg_sizes], fontsize=10)
ax1.tick_params(axis='y', labelsize=10)
ax1.grid(True, linestyle='--', alpha=0.6)
ax1.legend(fontsize=9, loc='upper left', framealpha=0.9)

# ---- Right subplot: LLC Load Misses ---------------------------------------
ax2.plot(msg_sizes, llc_misses_a1,
         marker='o', linewidth=2, markersize=8,
         color='#e74c3c', label='A1: Two-Copy (send/recv)')

ax2.plot(msg_sizes, llc_misses_a2,
         marker='s', linewidth=2, markersize=8,
         color='#2ecc71', label='A2: One-Copy (sendmsg + iovec)')

ax2.plot(msg_sizes, llc_misses_a3,
         marker='^', linewidth=2, markersize=8,
         color='#3498db', label='A3: Zero-Copy (MSG_ZEROCOPY)')

ax2.set_xlabel('Message Size (bytes)', fontsize=12, fontweight='bold')
ax2.set_ylabel('LLC Load Misses', fontsize=12, fontweight='bold')
ax2.set_title('LLC Load Misses vs Message Size', fontsize=13, fontweight='bold')
ax2.set_xscale('log', base=2)
ax2.set_xticks(msg_sizes)
ax2.set_xticklabels([str(s) for s in msg_sizes], fontsize=10)
ax2.tick_params(axis='y', labelsize=10)
ax2.grid(True, linestyle='--', alpha=0.6)
ax2.legend(fontsize=9, loc='upper left', framealpha=0.9)

# Super title
fig.suptitle('Cache Misses vs Message Size — Network I/O Primitives Comparison',
             fontsize=14, fontweight='bold', y=1.02)

# System configuration annotation
fig.text(0.5, -0.04, system_config,
         ha='center', fontsize=9, style='italic', color='gray')

plt.tight_layout()

plt.savefig('MT25082_cache_misses.png', dpi=200, bbox_inches='tight')
print('Saved: MT25082_cache_misses.png')
