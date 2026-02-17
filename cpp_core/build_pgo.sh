#!/bin/bash
# OPT-16: Profile-Guided Optimization build script
# Usage: cd cpp_core && bash build_pgo.sh
#
# Step 1: Build with instrumentation → generates profiling counters
# Step 2: Run benchmarks to generate profile data (.gcda files)
# Step 3: Rebuild using profile data → optimized branch prediction + icache layout

set -e

BUILD_DIR="build"
NPROC=$(nproc 2>/dev/null || echo 4)

echo "=== PGO Step 1: Build with instrumentation ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_GENERATE=ON -DENABLE_PGO_USE=OFF
make -j"$NPROC"
cp _fastapi_core*.so ../../fastapi/ 2>/dev/null || true
cd ..

echo "=== PGO Step 2: Run benchmarks to generate profile data ==="
echo "  Running test_app.py to generate HTTP + WS profile data..."
cd ..
timeout 10 python test_app.py &
sleep 2

# Generate HTTP traffic
if command -v wrk &>/dev/null; then
    wrk -t2 -c10 -d5s http://127.0.0.1:8000/json 2>/dev/null || true
elif command -v curl &>/dev/null; then
    for i in $(seq 1 1000); do
        curl -s http://127.0.0.1:8000/json >/dev/null 2>&1 || true
    done
fi

# Generate WS traffic
if command -v python3 &>/dev/null; then
    python3 -c "
import asyncio, websockets
async def bench():
    try:
        async with websockets.connect('ws://127.0.0.1:8000/ws/echo') as ws:
            for _ in range(1000):
                await ws.send('hello')
                await ws.recv()
    except: pass
asyncio.run(bench())
" 2>/dev/null || true
fi

# Stop server
kill %1 2>/dev/null || true
wait 2>/dev/null || true
cd cpp_core

echo "=== PGO Step 3: Rebuild with profile data ==="
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON
make -j"$NPROC"
cp _fastapi_core*.so ../../fastapi/ 2>/dev/null || true
cd ..

echo "=== PGO build complete! ==="
echo "Optimized module at: fastapi/_fastapi_core*.so"
