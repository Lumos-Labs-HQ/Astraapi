"""Test server backpressure handling under load."""
import asyncio
import httpx
import time

async def flood_request(client, url, data):
    """Send a single request."""
    try:
        response = await client.post(url, json=data, timeout=120.0)
        return response.status_code
    except Exception as e:
        return f"{type(e).__name__}: {str(e)[:100]}"

async def test_backpressure():
    """Flood server with concurrent requests to test backpressure."""
    url = "http://localhost:8002/post_large"
    
    # Large payload to stress the server
    payload = {
        "user_id": 1,
        "username": "test" * 500,
        "email": "test@example.com",
        "first_name": "Test" * 200,
        "last_name": "User" * 200,
        "age": 25,
        "address": {"street": "123 Main St" * 100, "city": "Test City" * 50, "data": "x" * 10000},
        "metadata": {f"key_{i}": f"value_{i}" * 50 for i in range(200)},
        "tags": ["tag" * 50 for _ in range(500)],
        "is_active": True,
        "created_at": "2024-01-01T00:00:00Z",
        "updated_at": "2024-01-01T00:00:00Z",
        "extra_data": "x" * 50000
    }
    
    print(f"Payload size: ~{len(str(payload))} bytes")
    print("Starting aggressive backpressure test...")
    print("Sending 500 concurrent requests with large payloads...")
    
    start = time.time()
    
    limits = httpx.Limits(max_connections=100, max_keepalive_connections=50)
    async with httpx.AsyncClient(limits=limits) as client:
        tasks = [flood_request(client, url, payload) for _ in range(500)]
        results = await asyncio.gather(*tasks, return_exceptions=True)
    
    elapsed = time.time() - start
    
    # Analyze results
    success = sum(1 for r in results if r == 200)
    errors = sum(1 for r in results if r != 200)
    
    print(f"\nResults:")
    print(f"  Total requests: {len(results)}")
    print(f"  Successful: {success}")
    print(f"  Errors: {errors}")
    print(f"  Time: {elapsed:.2f}s")
    print(f"  Throughput: {len(results)/elapsed:.2f} req/s")
    
    # Check for common error types
    error_types = {}
    for r in results:
        if r != 200:
            error_type = type(r).__name__ if isinstance(r, Exception) else str(r)
            error_types[error_type] = error_types.get(error_type, 0) + 1
    
    if error_types:
        print(f"\nError breakdown:")
        for err, count in error_types.items():
            print(f"  {err}: {count}")

if __name__ == "__main__":
    asyncio.run(test_backpressure())
