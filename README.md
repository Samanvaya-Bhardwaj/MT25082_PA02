# Roll No: MT25082

## PA02 — Analysis of Network I/O Primitives Using `perf`

**Course:** CSE638 — Graduate Systems  
**Assignment:** Programming Assignment 02  
**Author:** MT25082

---

## Table of Contents

1. [Overview](#overview)
2. [Problem Statement](#problem-statement)
3. [System Configuration](#system-configuration)
4. [Architecture & Design](#architecture--design)
   - [Common Infrastructure](#common-infrastructure)
   - [Part A1: Two-Copy Baseline](#part-a1-two-copy-baseline-sendrecv)
   - [Part A2: One-Copy Optimised](#part-a2-one-copy-optimised-sendmsg--iovec)
   - [Part A3: Zero-Copy](#part-a3-zero-copy-sendmsg--msg_zerocopy)
   - [Client Design](#client-design)
5. [File Listing](#file-listing)
6. [Prerequisites](#prerequisites)
7. [Building](#building)
8. [Running Experiments](#running-experiments)
   - [What the Script Does](#what-the-script-does)
   - [Network Namespace Setup](#network-namespace-setup)
   - [Cleanup & Safety](#cleanup--safety)
9. [Generating Plots](#generating-plots)
10. [Output Files](#output-files)
11. [CSV Format](#csv-format)
12. [Experimental Results Summary](#experimental-results-summary)
13. [Performance Analysis](#performance-analysis)
    - [Throughput Analysis](#throughput-analysis)
    - [Latency Analysis](#latency-analysis)
    - [CPU Cycles per Byte](#cpu-cycles-per-byte)
    - [Cache Behaviour](#cache-behaviour)
14. [Key Observations](#key-observations)
15. [Why Zero-Copy Underperforms on Loopback](#why-zero-copy-underperforms-on-loopback)
16. [Troubleshooting](#troubleshooting)

---

## Overview

This project benchmarks three TCP socket I/O strategies on Linux and measures
their performance using hardware counters via `perf stat`. The goal is to
understand how different data-copy semantics — **two-copy**, **one-copy**, and
**zero-copy** — affect throughput, latency, CPU utilisation, and cache
behaviour when transmitting structured messages over a network.

| Part | Strategy  | Key System Call                                                         |
| ---- | --------- | ----------------------------------------------------------------------- |
| A1   | Two-Copy  | `send()` / `recv()` — one `send()` per struct field                     |
| A2   | One-Copy  | `sendmsg()` with pre-registered `iovec[8]` — single scatter-gather call |
| A3   | Zero-Copy | `sendmsg()` + `MSG_ZEROCOPY` + `SO_EE_ORIGIN_ZEROCOPY` error queue      |

Each server sends a `message_t` struct containing **8 dynamically allocated
`char*` fields** to connected clients. The experiments sweep over:

- **4 message sizes:** 64, 256, 1024, 4096 bytes
- **4 thread counts:** 1, 2, 4, 8

This produces **48 unique configurations** (3 × 4 × 4), each measured for
throughput, latency, CPU cycles, L1/LLC cache misses, and context switches.

---

## Problem Statement

Modern network applications must balance throughput, latency, and CPU
efficiency. The Linux kernel provides multiple APIs for sending data over
TCP sockets, each with different copy semantics:

1. **`send()`** — The traditional approach. Data is copied from user space
   into a kernel socket buffer (sk_buff), then the kernel copies it to the
   NIC via DMA. This results in **two data copies** per message.

2. **`sendmsg()` with `iovec`** — Scatter-gather I/O. Multiple user-space
   buffers are gathered into a single kernel buffer in **one system call**,
   reducing syscall overhead while still performing **one kernel copy**.

3. **`sendmsg()` with `MSG_ZEROCOPY`** — The kernel pins user-space pages
   and instructs the NIC to DMA directly from them, achieving **zero data
   copies**. Completion notifications are delivered via the socket error queue.

This assignment implements all three approaches and quantifies their
performance differences using Linux `perf stat` hardware counters.

---

## System Configuration

All experiments were conducted on the following system:

| Component            | Specification                                    |
| -------------------- | ------------------------------------------------ |
| **Operating System** | Ubuntu, Linux 6.14.0-33-generic                  |
| **CPU**              | 13th Gen Intel Core i7-13700H (hybrid P+E cores) |
| **RAM**              | 16 GB                                            |
| **Network**          | `veth` pair inside Linux network namespaces      |
| **Compiler**         | `gcc` with `-O2 -Wall -Wextra -pthread`          |
| **Duration**         | 10 seconds per experiment configuration          |
| **Kernel support**   | `MSG_ZEROCOPY` (requires Linux ≥ 4.14 for TCP)   |

The network topology uses two Linux network namespaces
(`pa02_server_ns` and `pa02_client_ns`) connected by a `veth` pair with IP
addresses `10.0.0.1` (server) and `10.0.0.2` (client). This provides an
isolated, reproducible loopback-like environment while exercising the full
TCP/IP networking stack.

---

## Architecture & Design

### Common Infrastructure

All three implementations share a common header (`MT25082_common.h`) and
utility module (`MT25082_common.c`) that provides:

#### Data Structures

```c
#define NUM_FIELDS 8

typedef struct {
    char *field[NUM_FIELDS];    /* 8 dynamically allocated string buffers */
} message_t;

typedef struct {
    int    sock_fd;             /* Connected socket file descriptor */
    size_t msg_size;            /* Total message payload size (bytes) */
    int    duration_sec;        /* Duration of continuous transfer */
} thread_args_t;
```

The `message_t` struct deliberately uses **8 separate heap-allocated fields**
(not a contiguous buffer) to simulate a realistic scatter-gather scenario
where data resides in non-contiguous memory regions. The total `msg_size` is
distributed evenly across the 8 fields, with any remainder assigned to the
last field.

#### Utility Functions

| Function             | Purpose                                                           |
| -------------------- | ----------------------------------------------------------------- |
| `allocate_message()` | Allocates memory for all 8 fields, distributing size evenly       |
| `fill_message()`     | Fills each field with a distinct character pattern (A–H)          |
| `free_message()`     | Frees all 8 fields and NULLs the pointers                         |
| `get_time_us()`      | Microsecond-resolution timer via `clock_gettime(CLOCK_MONOTONIC)` |

### Part A1: Two-Copy Baseline (`send`/`recv`)

The two-copy implementation sends each of the 8 message fields individually
using a separate `send()` system call. This is the simplest but least
efficient approach.

**Data path:**

```
User Space              Kernel Space              Hardware
+-----------+  Copy 1   +----------------+  Copy 2  +--------+
| message_t | --------> | Socket send    | -------> |  NIC   |
| (heap)    |  send()   | buffer (sk_buf)|  DMA     | TX ring|
+-----------+           +----------------+          +--------+
```

**Characteristics:**

- **8 system calls per message** — one `send()` per field
- **Two data copies** — (1) user buffer → kernel socket buffer via `send()`,
  (2) kernel socket buffer → NIC via DMA
- Each `send()` call requires a full user→kernel context switch
- Partial sends are handled with a retry loop
- `MSG_NOSIGNAL` flag prevents SIGPIPE on broken connections

**Core send loop:**

```c
for (int i = 0; i < NUM_FIELDS; i++) {
    size_t field_size = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);
    size_t bytes_sent = 0;
    while (bytes_sent < field_size) {
        ssize_t ret = send(client_fd,
                           msg.field[i] + bytes_sent,
                           field_size   - bytes_sent,
                           MSG_NOSIGNAL);
        if (ret <= 0) { /* error handling */ break; }
        bytes_sent += (size_t)ret;
    }
}
```

### Part A2: One-Copy Optimised (`sendmsg` + `iovec`)

The one-copy implementation pre-registers all 8 fields in an `iovec[8]`
array and sends them in a **single `sendmsg()` system call** using
scatter-gather I/O.

**Data path:**

```
User Space                    Kernel Space              Hardware
+-----------+                 +----------------+         +--------+
| iovec[0]  |---+             |                |  DMA    |        |
| iovec[1]  |---+ sendmsg()  | sk_buff chain  | ------> |  NIC   |
| ...       |---+ (1 syscall) | (consolidated) |         | TX ring|
| iovec[7]  |---+             |                |         |        |
+-----------+                 +----------------+         +--------+
```

**Characteristics:**

- **1 system call per message** — single `sendmsg()` with scatter-gather
- **One data copy** — the kernel consolidates all `iovec` entries into a
  single `sk_buff` chain in one pass
- The `iovec` array and `msghdr` are set up once before the send loop,
  not re-initialised each iteration
- Eliminates the 8× syscall overhead of A1

**Core send loop:**

```c
struct iovec iov[NUM_FIELDS];
for (int i = 0; i < NUM_FIELDS; i++) {
    iov[i].iov_base = msg.field[i];
    iov[i].iov_len  = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);
}

struct msghdr mh = { .msg_iov = iov, .msg_iovlen = NUM_FIELDS };

while (g_running) {
    ssize_t ret = sendmsg(client_fd, &mh, MSG_NOSIGNAL);
    if (ret <= 0) { /* error handling */ break; }
    total_bytes_sent += (size_t)ret;
}
```

### Part A3: Zero-Copy (`sendmsg` + `MSG_ZEROCOPY`)

The zero-copy implementation extends A2 by adding the `MSG_ZEROCOPY` flag.
The kernel pins user-space pages and lets the NIC DMA directly from them,
**eliminating the user→kernel data copy entirely**.

**Data path:**

```
User Space                    Kernel Space                Hardware
+-----------+                 +------------------+        +--------+
| iovec[0]  |--+              |                  |        |        |
| iovec[1]  |--+ sendmsg()   | Page table pin   |  DMA   |        |
| ...       |--+ MSG_ZEROCOPY| (no memcpy!)     | -----> |  NIC   |
| iovec[7]  |--+              |                  |        | TX ring|
+-----------+                 +------------------+        +--------+
```

**Characteristics:**

- **1 system call per message** — `sendmsg()` with `MSG_ZEROCOPY`
- **Zero data copies** — page pinning + DMA from user pages
- Requires `SO_ZEROCOPY` socket option to be enabled
- Requires Linux kernel ≥ 4.14 for TCP zero-copy support
- **Completion notifications** must be drained from the socket error queue
  via `recvmsg(MSG_ERRQUEUE)` with `SO_EE_ORIGIN_ZEROCOPY`
- `ENOBUFS` handling — when too many pages are pinned, the kernel returns
  `ENOBUFS`; the implementation drains completions and retries

**Completion drain mechanism:**

The zero-copy send path generates asynchronous completion notifications.
These are delivered via the socket's error queue and must be drained
periodically to free pinned pages. The implementation uses a threshold
(`ZC_DRAIN_THRESHOLD = 256`) to batch drain operations:

```c
while (g_running) {
    ssize_t ret = sendmsg(client_fd, &mh, MSG_ZEROCOPY | MSG_NOSIGNAL);
    if (ret < 0 && errno == ENOBUFS) {
        drain_completions(client_fd, &pending_zc);
        usleep(100);
        continue;
    }
    total_bytes_sent += (size_t)ret;
    pending_zc++;
    if (pending_zc >= ZC_DRAIN_THRESHOLD)
        drain_completions(client_fd, &pending_zc);
}
```

The `drain_completions()` function reads `sock_extended_err` structures from
the error queue, extracting the `ee_data` (lo) and `ee_info` (hi) range to
determine how many send operations have completed.

### Client Design

All three clients (A1, A2, A3) share an **identical receive path** using
`recv()`, since the copy optimisations are purely on the **send side**
(server). Each client:

1. **Spawns N threads**, each opening its own TCP connection to the server
2. **Receives data** in a tight loop for the specified duration using
   `clock_gettime(CLOCK_MONOTONIC)` as a deadline
3. **Handles partial receives** — reassembles complete messages by tracking
   `bytes_in_msg` across multiple `recv()` calls
4. **Sets `TCP_NODELAY`** — disables Nagle's algorithm to avoid batching
   delays that would distort latency measurements
5. **Reports per-thread and aggregate metrics** — throughput (Gbps) and
   average latency (µs/message)

**Aggregate throughput** is computed as:

```
throughput_gbps = (total_bytes × 8) / (wall_clock_seconds × 1e9)
```

**Average latency** is computed as:

```
avg_latency_us = wall_clock_us / total_messages
```

---

## File Listing

| File                              | Description                                                   |
| --------------------------------- | ------------------------------------------------------------- |
| `MT25082_common.h`                | Shared header — structs, constants, function declarations     |
| `MT25082_common.c`                | Utility functions (allocate/fill/free message, `get_time_us`) |
| `MT25082_Part_A1_Server.c`        | A1 server — two-copy `send()` per field                       |
| `MT25082_Part_A1_Client.c`        | A1 client — `recv()` with partial-receive handling            |
| `MT25082_Part_A2_Server.c`        | A2 server — one-copy `sendmsg()` with `iovec`                 |
| `MT25082_Part_A2_Client.c`        | A2 client — identical receive path                            |
| `MT25082_Part_A3_Server.c`        | A3 server — zero-copy `MSG_ZEROCOPY` + error queue drain      |
| `MT25082_Part_A3_Client.c`        | A3 client — identical receive path                            |
| `Makefile`                        | Builds all 6 binaries with `gcc -O2 -Wall -pthread`           |
| `MT25082_run_experiments.sh`      | Automated experiment runner (48 combinations)                 |
| `MT25082_plot_throughput.py`      | Throughput vs message size plot (hardcoded data)              |
| `MT25082_plot_latency.py`         | Latency vs thread count plot (hardcoded data)                 |
| `MT25082_plot_cache_misses.py`    | L1 & LLC cache misses vs message size plot (hardcoded data)   |
| `MT25082_plot_cycles_per_byte.py` | CPU cycles per byte vs message size plot (hardcoded data)     |
| `MT25082_results.csv`             | Experimental results (generated by the script)                |
| `MT25082_PA02_Report.tex`         | LaTeX report with full analysis and figures                   |
| `README.md`                       | This file                                                     |

---

## Prerequisites

Before running the experiments, ensure the following are installed:

### Required

- **GCC** (C compiler with C99+ and POSIX thread support)
- **Linux kernel ≥ 4.14** (for `MSG_ZEROCOPY` TCP support)
- **Root access** (for network namespaces and `perf stat`)
- **iproute2** (`ip` command for namespace management)
- **make** (GNU Make for building)

### Optional (for plotting)

- **Python 3** with `matplotlib` package

### Installing dependencies (Ubuntu/Debian)

```bash
# Build tools
sudo apt-get install -y build-essential

# Linux perf (match your kernel version)
sudo apt-get install -y linux-tools-$(uname -r) linux-tools-generic

# Python plotting (optional)
pip3 install matplotlib
```

### Verifying perf availability

```bash
# Should print version info without errors
perf stat -e cycles -- true
```

If `perf` reports a kernel version mismatch, install the matching
`linux-tools-$(uname -r)` package. The experiment script will automatically
search `/usr/lib/linux-tools/*/perf` for a working binary if the default
`perf` command fails.

---

## Building

```bash
make clean && make
```

This compiles all 6 binaries:

| Binary              | Source Files                                    |
| ------------------- | ----------------------------------------------- |
| `MT25082_A1_Server` | `MT25082_Part_A1_Server.c` + `MT25082_common.c` |
| `MT25082_A1_Client` | `MT25082_Part_A1_Client.c` + `MT25082_common.c` |
| `MT25082_A2_Server` | `MT25082_Part_A2_Server.c` + `MT25082_common.c` |
| `MT25082_A2_Client` | `MT25082_Part_A2_Client.c` + `MT25082_common.c` |
| `MT25082_A3_Server` | `MT25082_Part_A3_Server.c` + `MT25082_common.c` |
| `MT25082_A3_Client` | `MT25082_Part_A3_Client.c` + `MT25082_common.c` |

Compiler flags: `-O2 -Wall -pthread`

To build manually (without Make):

```bash
gcc -O2 -Wall -pthread -o MT25082_A1_Server MT25082_Part_A1_Server.c MT25082_common.c -pthread
gcc -O2 -Wall -pthread -o MT25082_A1_Client MT25082_Part_A1_Client.c MT25082_common.c -pthread
# ... similarly for A2 and A3
```

---

## Running Experiments

### Quick Start

```bash
chmod +x MT25082_run_experiments.sh
sudo ./MT25082_run_experiments.sh
```

The script is **fully automated** — no user interaction is required after
launch. It will run for approximately **8–10 minutes** (48 experiments ×
10 seconds each + setup/teardown overhead).

### What the Script Does

The experiment script (`MT25082_run_experiments.sh`) performs the following
steps in order:

1. **Validates root privileges** — Exits with a clear error if not run
   with `sudo`.

2. **Kills stale processes** — Uses `pkill -9` to terminate any
   server/client binaries leftover from a previous aborted run. This
   prevents port conflicts and zombie processes.

3. **Deletes old output files** — Removes any existing `MT25082_results.csv`,
   `MT25082_perf_*.txt`, and `MT25082_client_*.txt` files to ensure a
   clean run.

4. **Cleans up old namespaces** — Deletes any leftover network namespaces
   from a previous run (`ip netns del`).

5. **Compiles all binaries** — Runs `make clean && make all` to ensure
   fresh builds.

6. **Sets up network namespaces** — Creates the `veth` pair and configures
   IP addresses (see below).

7. **Runs all 48 experiments** — Iterates over every combination of:
   - Implementation: A1, A2, A3
   - Message size: 64, 256, 1024, 4096 bytes
   - Thread count: 1, 2, 4, 8

   For each experiment:
   - Starts the server in the server namespace (background process)
   - Waits 2 seconds for the server to bind and listen
   - Runs the client wrapped in `perf stat` in the client namespace
   - Kills the server after the client finishes
   - Parses throughput, latency, and all `perf` counters
   - Appends a row to the master CSV file

8. **Prints a summary** — Displays the complete CSV on stdout.

9. **Cleans up** — The `trap EXIT` handler kills any remaining servers
   and deletes the network namespaces.

### Network Namespace Setup

The script creates an isolated network environment using Linux namespaces:

```
┌─────────────────────┐         ┌─────────────────────┐
│  pa02_server_ns     │         │  pa02_client_ns      │
│                     │         │                      │
│  veth-srv           │◄───────►│  veth-cli            │
│  10.0.0.1/24        │  veth   │  10.0.0.2/24         │
│                     │  pair   │                      │
│  [Server Binary]    │         │  [perf stat           │
│                     │         │    Client Binary]     │
└─────────────────────┘         └──────────────────────┘
```

This provides:

- **Network isolation** — no interference from other system traffic
- **Full TCP/IP stack traversal** — unlike `localhost`, the `veth` pair
  exercises the complete networking path
- **Reproducibility** — identical network conditions for every experiment

### Cleanup & Safety

The script includes multiple safety mechanisms:

- **`set -euo pipefail`** — exits on any error, undefined variable, or
  pipeline failure
- **`trap EXIT`** — always cleans up namespaces and kills servers, even
  if the script is interrupted with Ctrl+C
- **Startup process kill** — `pkill -9` on all 6 binary names to clear
  stale processes before starting
- **Idempotent namespace cleanup** — `ip netns del` with `|| true` so
  it doesn't error if namespaces don't exist
- **Server health check** — verifies each server is running with `kill -0`
  before starting the client
- **`SIGPIPE` ignored** — prevents servers from dying on broken connections

### Configuration

The following variables at the top of the script can be modified:

| Variable        | Default              | Purpose                            |
| --------------- | -------------------- | ---------------------------------- |
| `MSG_SIZES`     | `(64 256 1024 4096)` | Message sizes to test (bytes)      |
| `THREAD_COUNTS` | `(1 2 4 8)`          | Thread counts to test              |
| `DURATION`      | `10`                 | Seconds per experiment             |
| `PORT_A1`       | `9090`               | TCP port for A1 server             |
| `PORT_A2`       | `9091`               | TCP port for A2 server             |
| `PORT_A3`       | `9092`               | TCP port for A3 server             |
| `PERF_EVENTS`   | _(see below)_        | Comma-separated `perf stat` events |

Default `perf` events collected:

```
cycles,L1-dcache-load-misses,LLC-load-misses,LLC-store-misses,context-switches
```

---

## Generating Plots

The plot scripts use **hardcoded values** from the actual experimental data
and do **not** read the CSV file at runtime. This ensures the plots are
always reproducible regardless of when or where they are generated.

```bash
python3 MT25082_plot_throughput.py
python3 MT25082_plot_latency.py
python3 MT25082_plot_cache_misses.py
python3 MT25082_plot_cycles_per_byte.py
```

Each script generates a corresponding `.png` file:

| Script                            | Output PNG                    | Description                     |
| --------------------------------- | ----------------------------- | ------------------------------- |
| `MT25082_plot_throughput.py`      | `MT25082_throughput.png`      | Throughput vs message size      |
| `MT25082_plot_latency.py`         | `MT25082_latency.png`         | Latency vs thread count         |
| `MT25082_plot_cycles_per_byte.py` | `MT25082_cycles_per_byte.png` | CPU cycles/byte vs message size |
| `MT25082_plot_cache_misses.py`    | `MT25082_cache_misses.png`    | L1 & LLC misses vs message size |

Plots use the `Agg` backend (headless rendering) so they can be generated
on systems without a display (e.g., SSH sessions).

---

## Output Files

After a successful run, the following files are produced:

### CSV Results

- **`MT25082_results.csv`** — Master CSV with all 48 experiment rows

### Per-Experiment Files

For each experiment `{impl}_sz{size}_t{threads}`:

- **`MT25082_perf_{impl}_sz{size}_t{threads}.txt`** — Raw `perf stat`
  output (stderr)
- **`MT25082_client_{impl}_sz{size}_t{threads}.txt`** — Client stdout
  (throughput, latency, per-thread stats)

Example:

```
MT25082_perf_A2_sz4096_t8.txt     # perf output for A2, 4096B, 8 threads
MT25082_client_A2_sz4096_t8.txt   # client output for A2, 4096B, 8 threads
```

---

## CSV Format

The master CSV file has the following columns:

| Column             | Type    | Description                              |
| ------------------ | ------- | ---------------------------------------- |
| `implementation`   | string  | A1, A2, or A3                            |
| `msg_size`         | integer | Message size in bytes (64–4096)          |
| `threads`          | integer | Thread count (1–8)                       |
| `throughput_gbps`  | float   | Aggregate throughput in Gbps             |
| `latency_us`       | float   | Average per-message latency in µs        |
| `cycles`           | integer | Total CPU cycles (from `perf stat`)      |
| `L1_cache_misses`  | integer | L1 data cache load misses                |
| `LLC_load_misses`  | integer | Last-Level Cache load misses             |
| `LLC_store_misses` | integer | Last-Level Cache store misses            |
| `context_switches` | integer | Voluntary + involuntary context switches |

---

## Experimental Results Summary

### Throughput (Gbps) — selected highlights

| Config           | A1 (Two-Copy) | A2 (One-Copy) | A3 (Zero-Copy) |
| ---------------- | ------------- | ------------- | -------------- |
| 64B, 1 thread    | 0.0359        | 0.2322        | 0.1472         |
| 1024B, 4 threads | 2.0965        | 14.4613       | 7.8109         |
| 4096B, 8 threads | 11.0725       | 71.5685       | 39.5920        |

### Latency (µs) — selected highlights

| Config           | A1 (Two-Copy) | A2 (One-Copy) | A3 (Zero-Copy) |
| ---------------- | ------------- | ------------- | -------------- |
| 64B, 1 thread    | 14.27         | 2.20          | 3.48           |
| 1024B, 4 threads | 3.91          | 0.57          | 1.05           |
| 4096B, 8 threads | 2.96          | 0.46          | 0.83           |

---

## Performance Analysis

### Throughput Analysis

- **A2 (One-Copy)** achieves the highest throughput at every message size,
  peaking at **71.57 Gbps** with 4096-byte messages and 8 threads. The
  single `sendmsg()` call with scatter-gather `iovec` eliminates the
  per-field syscall overhead of A1.

- **A3 (Zero-Copy)** achieves the second-highest throughput (39.59 Gbps at
  4096B, 8 threads). On a `veth` loopback path, the `MSG_ZEROCOPY`
  completion notification overhead exceeds the savings from eliminating
  the user→kernel copy.

- **A1 (Two-Copy)** has the lowest throughput across all configurations
  (maximum 11.07 Gbps), limited by 8 separate `send()` system calls per
  message.

- Throughput scales roughly **linearly with message size** because the
  amortised syscall cost per byte decreases with larger payloads.

### Latency Analysis

- **A1** exhibits dramatically higher latency (14–15 µs with 1 thread)
  because each message requires 8 individual `send()` syscalls, each
  involving a user→kernel context switch.

- **A2** achieves the lowest latency (0.38 µs at 8 threads for small
  messages), benefiting from the single syscall per message.

- **A3** has marginally higher latency than A2 (0.70–3.56 µs) due to
  `MSG_ZEROCOPY` completion notification overhead.

- Latency **decreases with increasing thread count** because the load is
  distributed across parallel connections, reducing per-thread queueing.

### CPU Cycles per Byte

- **A1** is the most CPU-intensive, consuming ~644 cycles/byte for
  64-byte messages due to per-field `send()` overhead.

- **A2 and A3** converge to ~2 cycles/byte for 4096-byte messages.

- CPU efficiency improves by **~60×** from 64B to 4096B (A1), reflecting
  amortisation of fixed per-syscall costs.

### Cache Behaviour

- **L1 cache misses** are comparable across all implementations for small
  messages. For 4096B, A2 shows higher L1 misses due to processing more
  data volume (higher throughput = more bytes in the same time window).

- **LLC load misses** are notably higher for A2 at large message sizes
  (up to ~4.9M for 4096B, 8 threads), reflecting the larger working set
  from scatter-gather I/O. This is a throughput-driven effect, not an
  inefficiency.

- A3 maintains the **lowest LLC miss counts** across all sizes, suggesting
  zero-copy page pinning avoids kernel-side buffer allocation that causes
  LLC pressure in A2.

---

## Key Observations

1. **Scatter-gather I/O (`sendmsg` + `iovec`)** provides the best overall
   performance on loopback, achieving up to 71.57 Gbps by consolidating
   8 buffer segments into a single syscall.

2. **Zero-copy (`MSG_ZEROCOPY`)** eliminates user→kernel copies but
   introduces completion notification overhead that negates the benefit
   on loopback paths.

3. **Per-field `send()` calls** (A1) are the worst strategy — syscall
   overhead dominates at small message sizes (644 cycles/byte for 64B
   vs. 101 cycles/byte for A2).

4. **Message size is the strongest predictor** of efficiency —
   amortising fixed syscall costs over larger payloads improves
   throughput by ~60× (64B → 4096B for A1).

5. **Multi-threading** provides near-linear throughput scaling across
   all implementations, confirming the bottleneck is per-connection
   syscall overhead rather than kernel-level contention.

---

## Why Zero-Copy Underperforms on Loopback

Intuitively, zero-copy (A3) should outperform one-copy (A2). However, on
a `veth` loopback path, A3 consistently underperforms A2 because:

1. **No real DMA savings** — On a `veth` pair, data never leaves RAM.
   Zero-copy's benefit (NIC DMA from user pages) provides no advantage
   when no hardware DMA engine is involved.

2. **Completion notification overhead** — Each zero-copy send generates
   a completion notification on the socket error queue. The
   `drain_completions()` function must call `recvmsg(MSG_ERRQUEUE)`
   periodically, adding extra syscalls per message.

3. **Page pinning cost** — The kernel must pin user-space pages (increment
   page reference counts, flush TLB entries), which costs more than a
   simple `memcpy` for small-to-medium messages.

Zero-copy would provide significantly more benefit with a **real NIC**
performing DMA, particularly for **very large messages (≥ 16 KB)** where
the copy savings outweigh the notification overhead.

---

## Troubleshooting

### `perf stat: not available`

```
perf stat    : NOT AVAILABLE (will run without hardware counters)
```

**Fix:** Install perf tools matching your kernel:

```bash
sudo apt-get install linux-tools-$(uname -r)
```

### `setsockopt SO_ZEROCOPY: Protocol not available`

Your kernel does not support `MSG_ZEROCOPY` for TCP. Requires Linux ≥ 4.14.
Check with:

```bash
uname -r
```

### `bind: Address already in use`

A previous server process is still holding the port. The script handles
this automatically with `pkill -9` at startup, but you can also manually:

```bash
sudo pkill -9 -f MT25082_A1_Server
sudo pkill -9 -f MT25082_A2_Server
sudo pkill -9 -f MT25082_A3_Server
```

### `Cannot reach server namespace`

The `veth` pair setup failed. This can happen if old namespaces exist.
Clean them up:

```bash
sudo ip netns del pa02_server_ns 2>/dev/null
sudo ip netns del pa02_client_ns 2>/dev/null
```

### Hybrid CPU `perf` output

On Intel Alder Lake / Raptor Lake CPUs, `perf stat` may split events into
`cpu_atom/...` and `cpu_core/...` lines, some showing `<not supported>`.
The parsing function handles this by finding the **largest valid numeric
value** across matching lines and skipping `<not supported>` entries.

---
