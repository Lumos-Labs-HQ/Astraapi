#!/bin/bash
# PGO (Profile-Guided Optimization) build
# Steps: instrument → profile under real load → rebuild with data
#
# Usage:  cd <repo-root>  &&  bash cpp_core/build_pgo.sh
# Or cd cpp_core && bash build_pgo.sh
# The script auto-detects both invocation styles.

set -e

# ── Locate repo root ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

cd "$SCRIPT_DIR"

# ── Step 1: Instrumented build ────────────────────────────────────────────────
echo "=== PGO Step 1: Build with instrumentation (generates .gcda counters) ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_GENERATE=ON -DENABLE_PGO_USE=OFF -DCMAKE_VERBOSE_MAKEFILE=OFF
make -j"$NPROC"
cp _fastapi_core*.so "$REPO_ROOT/fastapi/" 2>/dev/null || true
cd "$SCRIPT_DIR"

# ── Step 2: Warm under real benchmark load to generate profile data ────────────
echo "=== PGO Step 2: Profiling under real workload ==="
cd "$REPO_ROOT"

# Start server in background (instruments both sync + async hot paths)
python test.py &
SERVER_PID=$!
echo "  Server PID: $SERVER_PID"
sleep 1.5  # wait for startup

PORT=8000

# Warm up first (fills icache, ensures routes registered)
echo "  Warming up..."
for _ in $(seq 1 200); do
    curl -s -o /dev/null "http://127.0.0.1:$PORT/" || true
    curl -s -o /dev/null "http://127.0.0.1:$PORT/async" || true
done

# Profile sync path
if command -v wrk &>/dev/null; then
    echo "  Profiling sync path..."
    wrk -t2 -c50 -d8s "http://127.0.0.1:$PORT/" >/dev/null 2>&1 || true
    echo "  Profiling async path..."
    wrk -t2 -c50 -d8s "http://127.0.0.1:$PORT/async" >/dev/null 2>&1 || true
else
    echo "  wrk not found, using curl for profiling..."
    for i in $(seq 1 5000); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/" &
        curl -s -o /dev/null "http://127.0.0.1:$PORT/async" &
        if (( i % 50 == 0 )); then wait; fi
    done
    wait
fi

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
echo "  Profiling complete."

cd "$SCRIPT_DIR"

# ── Step 3: Rebuild with profile data ────────────────────────────────────────
echo "=== PGO Step 3: Optimized build using profile data ==="
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON -DCMAKE_VERBOSE_MAKEFILE=OFF
make -j"$NPROC"
cp _fastapi_core*.so "$REPO_ROOT/fastapi/" 2>/dev/null || true
cd ..

echo ""
echo "=== PGO build complete! ==="
echo "  Module installed at: $REPO_ROOT/fastapi/_fastapi_core*.so"
echo "  Expected improvement: +5-20% throughput vs standard -O3 build"
echo ""
echo "  Benchmark with:"
echo "    wrk -c100 -d10s http://localhost:$PORT/"
echo "    wrk -c100 -d10s http://localhost:$PORT/async"

