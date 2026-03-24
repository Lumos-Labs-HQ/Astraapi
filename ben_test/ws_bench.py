"""WebSocket echo benchmark — measures messages/sec for each server."""
import asyncio, time, sys

async def bench_ws(url: str, label: str, msg_count: int = 10_000, concurrency: int = 10):
    import websockets

    sent = 0
    errors = 0
    payload = "hello world benchmark"

    async def worker():
        nonlocal sent, errors
        try:
            async with websockets.connect(url) as ws:
                for _ in range(msg_count // concurrency):
                    await ws.send(payload)
                    await ws.recv()
                    sent += 1
        except Exception as e:
            errors += 1

    t0 = time.perf_counter()
    await asyncio.gather(*[worker() for _ in range(concurrency)])
    elapsed = time.perf_counter() - t0

    rps = sent / elapsed
    print(f"{label:20s}  {rps:>10,.0f} msg/s  ({sent} msgs in {elapsed:.2f}s, errors={errors})")

async def main():
    tests = [
        ("ws://127.0.0.1:8002/ws", "fastapi-rust"),
        ("ws://127.0.0.1:4000/ws", "axum/rust"),
        ("ws://127.0.0.1:3000/ws", "hono/bun"),
    ]
    for url, label in tests:
        try:
            await bench_ws(url, label)
        except Exception as e:
            print(f"{label:20s}  FAILED: {e}")

asyncio.run(main())
