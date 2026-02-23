import time

t0 = time.perf_counter()
from fastapi import FastAPI
t1 = time.perf_counter()
print(f"1. from fastapi import FastAPI: {(t1-t0)*1000:.0f}ms")

t2 = time.perf_counter()
app = FastAPI()
t3 = time.perf_counter()
print(f"2. FastAPI(): {(t3-t2)*1000:.0f}ms")

t4 = time.perf_counter()
@app.get("/test")
async def test():
    return {"hello": "world"}
t5 = time.perf_counter()
print(f"3. @app.get (first route, triggers pydantic): {(t5-t4)*1000:.0f}ms")

t6 = time.perf_counter()
@app.get("/test2")
async def test2():
    return {"hello": "world2"}
t7 = time.perf_counter()
print(f"4. @app.get (second route, cached): {(t7-t6)*1000:.0f}ms")

# Simulate what app.run() does
import asyncio

t8 = time.perf_counter()
try:
    import uvloop
    asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())
except ImportError:
    try:
        import winloop
        asyncio.set_event_loop_policy(winloop.EventLoopPolicy())
    except ImportError:
        pass
t9 = time.perf_counter()
print(f"5. Event loop policy: {(t9-t8)*1000:.0f}ms")

t10 = time.perf_counter()
from fastapi._cpp_server import run_server
t11 = time.perf_counter()
print(f"6. import _cpp_server: {(t11-t10)*1000:.0f}ms")

t12 = time.perf_counter()
app._sync_routes_to_core()
t13 = time.perf_counter()
print(f"7. _sync_routes_to_core: {(t13-t12)*1000:.0f}ms")

print(f"\nTotal before server: {(t13-t0)*1000:.0f}ms")
print(f"Routes: {[r.path for r in app.routes]}")
print("Starting server...")
asyncio.run(run_server(app, "127.0.0.1", 8002))
