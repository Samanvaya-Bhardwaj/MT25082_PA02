#!/usr/bin/env bash
# Roll No: MT25082
# =============================================================================
# Script:  MT25082_run_experiments.sh
# Purpose: Fully automated experiment runner for PA02.
#
#          Compiles all implementations, sets up network namespaces, runs
#          each (implementation × message_size × thread_count) combination,
#          collects perf stat metrics, and writes results to CSV files.
#
#          NO user interaction is required after the script starts.
#
# Usage:
#   chmod +x MT25082_run_experiments.sh
#   sudo ./MT25082_run_experiments.sh
#
# Note:
#   Requires root (sudo) for:
#     • Network namespace creation (ip netns)
#     • perf stat (hardware counters)
# =============================================================================

set -euo pipefail

# =============================================================================
#  Configuration
# =============================================================================

# Message sizes to test (bytes)
MSG_SIZES=(64 256 1024 4096)

# Thread counts to test
THREAD_COUNTS=(1 2 4 8)

# Duration of each experiment run (seconds)
DURATION=10

# Base port for each implementation (avoids conflicts)
PORT_A1=9090
PORT_A2=9091
PORT_A3=9092

# Network namespace names
NS_SERVER="pa02_server_ns"
NS_CLIENT="pa02_client_ns"

# Veth pair names
VETH_SERVER="veth-srv"
VETH_CLIENT="veth-cli"

# IP addresses for the namespace endpoints
IP_SERVER="10.0.0.1"
IP_CLIENT="10.0.0.2"
SUBNET="/24"

# Directory where this script lives (all binaries & output go here)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Results directory
RESULTS_DIR="$SCRIPT_DIR"

# Master CSV file that aggregates all results
MASTER_CSV="$RESULTS_DIR/MT25082_results.csv"

# perf events to collect
PERF_EVENTS="cycles,L1-dcache-load-misses,LLC-load-misses,LLC-store-misses,context-switches"

# Implementation labels and binary mappings
declare -A SERVER_BIN
SERVER_BIN[A1]="./MT25082_A1_Server"
SERVER_BIN[A2]="./MT25082_A2_Server"
SERVER_BIN[A3]="./MT25082_A3_Server"

declare -A CLIENT_BIN
CLIENT_BIN[A1]="./MT25082_A1_Client"
CLIENT_BIN[A2]="./MT25082_A2_Client"
CLIENT_BIN[A3]="./MT25082_A3_Client"

declare -A IMPL_PORT
IMPL_PORT[A1]=$PORT_A1
IMPL_PORT[A2]=$PORT_A2
IMPL_PORT[A3]=$PORT_A3

IMPLEMENTATIONS=(A1 A2 A3)

# Check if perf is actually functional (kernel version must match)
# Try the default perf first; if it fails (kernel mismatch), search for
# any working perf binary under /usr/lib/linux-tools/.
PERF_CMD="perf"
PERF_AVAILABLE=false
if command -v perf &>/dev/null && perf stat -e cycles -- true &>/dev/null 2>&1; then
    PERF_AVAILABLE=true
else
    # Fallback: find a working perf binary from installed linux-tools
    for candidate in /usr/lib/linux-tools/*/perf; do
        if [[ -x "$candidate" ]] && "$candidate" stat -e cycles -- true &>/dev/null 2>&1; then
            PERF_CMD="$candidate"
            PERF_AVAILABLE=true
            break
        fi
    done
fi

# =============================================================================
#  Helper Functions
# =============================================================================

log() {
    # Timestamped log messages for progress tracking
    echo "[$(date '+%H:%M:%S')] $*"
}

cleanup_namespaces() {
    # Remove network namespaces and veth pairs if they exist.
    # Called at start (idempotent cleanup) and at exit.
    log "Cleaning up network namespaces …"
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    ip link del "$VETH_SERVER" 2>/dev/null || true
}

setup_namespaces() {
    # Create two network namespaces connected by a veth pair.
    # This simulates separate hosts for client and server as required
    # by the assignment (no VMs, use namespaces).
    log "Setting up network namespaces …"

    # Step 1: Create namespaces
    ip netns add "$NS_SERVER"
    ip netns add "$NS_CLIENT"

    # Step 2: Create a veth pair — two virtual NICs connected back-to-back
    ip link add "$VETH_SERVER" type veth peer name "$VETH_CLIENT"

    # Step 3: Move each end into its respective namespace
    ip link set "$VETH_SERVER" netns "$NS_SERVER"
    ip link set "$VETH_CLIENT" netns "$NS_CLIENT"

    # Step 4: Assign IP addresses
    ip netns exec "$NS_SERVER" ip addr add "${IP_SERVER}${SUBNET}" dev "$VETH_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "${IP_CLIENT}${SUBNET}" dev "$VETH_CLIENT"

    # Step 5: Bring interfaces up
    ip netns exec "$NS_SERVER" ip link set "$VETH_SERVER" up
    ip netns exec "$NS_CLIENT" ip link set "$VETH_CLIENT" up

    # Step 6: Bring up loopback in both namespaces
    ip netns exec "$NS_SERVER" ip link set lo up
    ip netns exec "$NS_CLIENT" ip link set lo up

    # Verify connectivity
    log "Verifying namespace connectivity …"
    ip netns exec "$NS_CLIENT" ping -c 1 -W 2 "$IP_SERVER" > /dev/null 2>&1 \
        && log "  Connectivity OK (${IP_CLIENT} → ${IP_SERVER})" \
        || { log "  ERROR: Cannot reach server namespace!"; exit 1; }
}

kill_servers() {
    # Kill any leftover server processes in the server namespace
    for impl in "${IMPLEMENTATIONS[@]}"; do
        local bin_name
        bin_name=$(basename "${SERVER_BIN[$impl]}")
        ip netns exec "$NS_SERVER" pkill -f "$bin_name" 2>/dev/null || true
    done
    sleep 1
}

parse_perf_output() {
    # Parse perf stat stderr output and extract metric values.
    # Arguments:
    #   $1 — path to perf stat output file
    #   $2 — metric name (e.g., "cycles", "L1-dcache-load-misses")
    # Returns: numeric value (commas stripped) or "0" if not found.
    #
    # perf on hybrid CPUs (e.g., Intel Alder Lake) splits events into
    # cpu_atom/... and cpu_core/... lines.  Some may show "<not supported>".
    # We find the largest numeric value across matching lines.
    local perf_file="$1"
    local metric="$2"
    local best=0

    while IFS= read -r line; do
        # Skip lines with "<not" (i.e., <not supported> or <not counted>)
        if echo "$line" | grep -q '<not'; then
            continue
        fi
        # Extract the first numeric token (strip commas)
        local val
        val=$(echo "$line" | awk '{gsub(/,/,"",$1); print $1}' 2>/dev/null)
        # Validate it's a number
        if [[ "$val" =~ ^[0-9]+$ ]] && (( val > best )); then
            best=$val
        fi
    done < <(grep -i "$metric" "$perf_file" 2>/dev/null)

    echo "$best"
}

parse_client_output() {
    # Parse client stdout for throughput and latency from the AGGREGATE
    # RESULTS section.
    # Arguments:
    #   $1 — path to client output file
    #   $2 — field name ("throughput" or "latency")
    local out_file="$1"
    local field="$2"

    if [[ "$field" == "throughput" ]]; then
        # Match the aggregate section line:  "Aggregate throughput : 0.6364 Gbps"
        grep "^Aggregate throughput" "$out_file" | head -1 \
            | awk -F':' '{print $2}' | grep -oP '[0-9]+\.[0-9]+' | head -1
    elif [[ "$field" == "latency" ]]; then
        # Match the aggregate section line:  "Avg latency/msg      : 12.87 µs"
        grep "^Avg latency" "$out_file" | head -1 \
            | awk -F':' '{print $2}' | grep -oP '[0-9]+\.[0-9]+' | head -1
    else
        echo "0"
    fi
}

# =============================================================================
#  Main Script
# =============================================================================

# Ensure we are running as root (needed for namespaces and perf)
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: This script must be run as root (sudo)."
    echo "  Usage: sudo ./MT25082_run_experiments.sh"
    exit 1
fi

log "===== PA02 Experiment Runner — MT25082 ====="
log "Message sizes : ${MSG_SIZES[*]}"
log "Thread counts : ${THREAD_COUNTS[*]}"
log "Duration      : ${DURATION}s per experiment"
if [[ "$PERF_AVAILABLE" == true ]]; then
    log "perf stat    : AVAILABLE ($PERF_CMD)"
else
    log "perf stat    : NOT AVAILABLE (will run without hardware counters)"
    log "  To fix: install linux-tools matching kernel $(uname -r)"
fi

# ---- Step 1: Clean previous runs (idempotent) -----------------------------
log "Cleaning previous results …"
rm -f "$MASTER_CSV"
rm -f "$RESULTS_DIR"/MT25082_perf_*.txt
rm -f "$RESULTS_DIR"/MT25082_client_*.txt

# Kill any stale server/client processes from a previous aborted run
log "Killing stale processes from previous runs …"
for impl in "${IMPLEMENTATIONS[@]}"; do
    pkill -9 -f "$(basename "${SERVER_BIN[$impl]}")" 2>/dev/null || true
    pkill -9 -f "$(basename "${CLIENT_BIN[$impl]}")" 2>/dev/null || true
done
sleep 1

cleanup_namespaces

# ---- Step 2: Compile everything -------------------------------------------
log "Compiling all implementations …"
make clean
make all
log "Compilation successful."

# ---- Step 3: Set up network namespaces ------------------------------------
setup_namespaces

# ---- Step 4: Write CSV header ---------------------------------------------
echo "implementation,msg_size,threads,throughput_gbps,latency_us,cycles,L1_cache_misses,LLC_load_misses,LLC_store_misses,context_switches" \
    > "$MASTER_CSV"

# ---- Step 5: Register cleanup on exit ------------------------------------
trap 'kill_servers; cleanup_namespaces; log "Cleanup complete."' EXIT

# ---- Step 6: Run experiments ----------------------------------------------
total_experiments=$(( ${#IMPLEMENTATIONS[@]} * ${#MSG_SIZES[@]} * ${#THREAD_COUNTS[@]} ))
current_experiment=0

for impl in "${IMPLEMENTATIONS[@]}"; do
    for msg_size in "${MSG_SIZES[@]}"; do
        for threads in "${THREAD_COUNTS[@]}"; do
            current_experiment=$((current_experiment + 1))
            port="${IMPL_PORT[$impl]}"

            log "────────────────────────────────────────────────────"
            log "Experiment ${current_experiment}/${total_experiments}: " \
                "${impl} | msg_size=${msg_size} | threads=${threads}"
            log "────────────────────────────────────────────────────"

            # Filenames encode experiment parameters as required
            perf_file="${RESULTS_DIR}/MT25082_perf_${impl}_sz${msg_size}_t${threads}.txt"
            client_file="${RESULTS_DIR}/MT25082_client_${impl}_sz${msg_size}_t${threads}.txt"

            # ---- Start server in server namespace ----------------------
            log "  Starting ${impl} server (port=${port}, msg_size=${msg_size}) …"
            ip netns exec "$NS_SERVER" \
                "${SERVER_BIN[$impl]}" "$port" "$msg_size" \
                > /dev/null 2>&1 &
            server_pid=$!

            # Give the server time to bind and listen
            sleep 2

            # Verify server is still running
            if ! kill -0 "$server_pid" 2>/dev/null; then
                log "  WARNING: Server failed to start, skipping …"
                wait "$server_pid" 2>/dev/null || true
                continue
            fi

            # ---- Run client with perf stat in client namespace ---------
            #
            # perf stat wraps the client process and collects hardware
            # performance counters for the entire client execution:
            #   • cycles           — total CPU cycles consumed
            #   • L1-dcache-load-misses — L1 data cache misses
            #   • LLC-load-misses  — Last-Level Cache load misses
            #   • LLC-store-misses — Last-Level Cache store misses
            #   • context-switches — voluntary + involuntary CS
            #
            # The -e flag specifies which events to monitor.
            # perf output goes to stderr → redirected to perf_file.
            # Client stdout (throughput, latency) → client_file.
            log "  Running ${impl} client (threads=${threads}, duration=${DURATION}s) …"
            if [[ "$PERF_AVAILABLE" == true ]]; then
                # Run with perf stat to collect hardware counters
                ip netns exec "$NS_CLIENT" \
                    "$PERF_CMD" stat -e "$PERF_EVENTS" \
                    "${CLIENT_BIN[$impl]}" "$IP_SERVER" "$port" "$msg_size" \
                        "$threads" "$DURATION" \
                    > "$client_file" 2> "$perf_file" || true
            else
                # Run without perf — collect app-level metrics only
                ip netns exec "$NS_CLIENT" \
                    "${CLIENT_BIN[$impl]}" "$IP_SERVER" "$port" "$msg_size" \
                        "$threads" "$DURATION" \
                    > "$client_file" 2>&1 || true
                # Create empty perf file so parsing doesn't fail
                echo "(perf not available)" > "$perf_file"
            fi

            # ---- Stop the server ---------------------------------------
            log "  Stopping server (pid=${server_pid}) …"
            kill "$server_pid" 2>/dev/null || true
            wait "$server_pid" 2>/dev/null || true
            sleep 1

            # ---- Parse results -----------------------------------------
            throughput=$(parse_client_output "$client_file" "throughput")
            latency=$(parse_client_output "$client_file" "latency")
            cycles=$(parse_perf_output "$perf_file" "cycles")
            l1_misses=$(parse_perf_output "$perf_file" "L1-dcache-load-misses")
            llc_load_misses=$(parse_perf_output "$perf_file" "LLC-load-misses")
            llc_store_misses=$(parse_perf_output "$perf_file" "LLC-store-misses")
            ctx_switches=$(parse_perf_output "$perf_file" "context-switches")

            # Default to 0 for any missing values
            throughput="${throughput:-0}"
            latency="${latency:-0}"
            cycles="${cycles:-0}"
            l1_misses="${l1_misses:-0}"
            llc_load_misses="${llc_load_misses:-0}"
            llc_store_misses="${llc_store_misses:-0}"
            ctx_switches="${ctx_switches:-0}"

            # ---- Append to master CSV ----------------------------------
            echo "${impl},${msg_size},${threads},${throughput},${latency},${cycles},${l1_misses},${llc_load_misses},${llc_store_misses},${ctx_switches}" \
                >> "$MASTER_CSV"

            log "  Results: throughput=${throughput} Gbps, " \
                "latency=${latency} µs, cycles=${cycles}, " \
                "L1_misses=${l1_misses}, LLC_load=${llc_load_misses}, " \
                "ctx_sw=${ctx_switches}"
        done
    done
done

# ---- Step 7: Summary ------------------------------------------------------
log ""
log "===== ALL EXPERIMENTS COMPLETE ====="
log "Master CSV : ${MASTER_CSV}"
log "perf files : ${RESULTS_DIR}/MT25082_perf_*.txt"
log "client logs: ${RESULTS_DIR}/MT25082_client_*.txt"
log ""
log "CSV contents:"
cat "$MASTER_CSV"

exit 0
