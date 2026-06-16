#!/bin/bash
# demo_ebpf.sh — Demonstrate eBPF off-CPU profiling under IO/scheduler pressure
#
# Prerequisites:
#   - drop_server and drop_agent running (or docker compose up)
#   - apiserver running
#   - stress-ng and dd available
#
# Usage:
#   ./scripts/demo_ebpf.sh [apiserver_addr]
#
# What it does:
#   1. Creates an eBPF off-CPU profiling task (baseline — idle system)
#   2. Triggers IO pressure via dd + schedule pressure via stress-ng
#   3. Creates a second eBPF profiling task during the pressure window
#   4. Waits for both tasks to complete analysis
#   5. Prints PreSign URLs for both Off-CPU flame graphs for comparison

set -euo pipefail

API="${1:-http://127.0.0.1:8191}"
COOKIE="drop_user_uid=demo;drop_user_name=demo_user"

echo "=== Drop eBPF Off-CPU Demo ==="
echo ""

# --- Helper: create an eBPF task ---
create_ebpf_task() {
    local name="$1"
    local pid="${2:-0}"
    local duration="${3:-10}"
    curl -sf -X POST "${API}/api/v1/tasks" \
        -b "${COOKIE}" \
        -H 'Content-Type: application/json' \
        -d "{
            \"name\": \"${name}\",
            \"type\": 0,
            \"profiler_type\": 3,
            \"target_ip\": \"127.0.0.1\",
            \"pid\": ${pid},
            \"duration\": ${duration},
            \"hz\": 0
        }" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('task',{}).get('tid',''))"
}

# --- Helper: wait for analysis to complete ---
wait_for_analysis() {
    local tid="$1"
    local max_wait=120
    local elapsed=0
    while [ $elapsed -lt $max_wait ]; do
        status=$(curl -sf "${API}/api/v1/tasks/${tid}" -b "${COOKIE}" \
            | python3 -c "import sys,json; d=json.load(sys.stdin); t=d.get('data',{}).get('task',{}); print(f\"{t.get('status',-1)} {t.get('analysis_status',-1)}\")")
        task_status=$(echo "$status" | awk '{print $1}')
        analysis_status=$(echo "$status" | awk '{print $2}')
        if [ "$task_status" = "3" ] && [ "$analysis_status" = "2" ]; then
            echo "  Task ${tid}: analysis complete"
            return 0
        fi
        if [ "$task_status" = "4" ]; then
            echo "  Task ${tid}: FAILED"
            return 1
        fi
        sleep 3
        elapsed=$((elapsed + 3))
    done
    echo "  Task ${tid}: timeout waiting for analysis"
    return 1
}

# Step 1: Baseline (idle system)
echo "[1/5] Creating baseline eBPF off-CPU task (idle system)..."
BASELINE_TID=$(create_ebpf_task "ebpf-baseline-idle" 0 10)
if [ -z "$BASELINE_TID" ]; then
    echo "ERROR: Failed to create baseline task"
    exit 1
fi
echo "  Baseline TID: ${BASELINE_TID}"

# Wait for baseline to finish collecting (we need to start pressure during step 2)
echo "[2/5] Waiting 12 seconds for baseline collection to complete..."
sleep 12

# Step 2: Start IO + scheduler pressure in background
echo "[3/5] Starting IO + scheduler pressure..."
# IO pressure: dd writing a large file
dd if=/dev/zero of=/tmp/drop_ebpf_bigfile bs=1M count=512 oflag=dsync 2>/dev/null &
DD_PID=$!
# Scheduler pressure: stress-ng
stress-ng --cpu 4 --io 2 --timeout 25s >/dev/null 2>&1 &
STRESS_PID=$!
echo "  dd PID=${DD_PID}, stress-ng PID=${STRESS_PID}"

# Wait a moment for pressure to build up
sleep 3

# Step 3: Create eBPF task under pressure
echo "[4/5] Creating eBPF off-CPU task (under IO/scheduler pressure)..."
PRESSURE_TID=$(create_ebpf_task "ebpf-under-pressure" 0 10)
if [ -z "$PRESSURE_TID" ]; then
    echo "ERROR: Failed to create pressure task"
    kill $DD_PID $STRESS_PID 2>/dev/null || true
    exit 1
fi
echo "  Pressure TID: ${PRESSURE_TID}"

# Wait for pressure task to complete
sleep 15

# Cleanup pressure processes
kill $DD_PID $STRESS_PID 2>/dev/null || true
rm -f /tmp/drop_ebpf_bigfile
echo "  Pressure processes stopped"

# Step 4: Wait for both tasks to complete analysis
echo "[5/5] Waiting for analysis to complete..."
wait_for_analysis "$BASELINE_TID" || true
wait_for_analysis "$PRESSURE_TID" || true

# Step 5: Print results
echo ""
echo "=== Results ==="
echo ""
echo "Baseline (idle system) Off-CPU flame graph:"
curl -sf "${API}/api/v1/tasks/${BASELINE_TID}/files" -b "${COOKIE}" \
    | python3 -c "
import sys, json
data = json.load(sys.stdin)
for f in data.get('data', {}).get('files', []):
    if 'offcpu' in f.get('name', ''):
        print(f\"  {f['name']}: {f['url']}\")
" || echo "  (could not retrieve)"

echo ""
echo "Pressure (IO+stress-ng) Off-CPU flame graph:"
curl -sf "${API}/api/v1/tasks/${PRESSURE_TID}/files" -b "${COOKIE}" \
    | python3 -c "
import sys, json
data = json.load(sys.stdin)
for f in data.get('data', {}).get('files', []):
    if 'offcpu' in f.get('name', ''):
        print(f\"  {f['name']}: {f['url']}\")
" || echo "  (could not retrieve)"

echo ""
echo "Compare the two flame graphs in your browser."
echo "The pressure flame graph should show IO wait / schedule related stacks"
echo "that are absent or minimal in the baseline."
