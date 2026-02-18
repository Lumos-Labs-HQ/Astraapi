"""
Complete test suite for the backend
Tests all features: validation, database, auth, middleware
"""
import asyncio
import httpx
import json

BASE_URL = "http://localhost:8003"

async def test_all():
    async with httpx.AsyncClient() as client:
        print("=" * 60)
        print("COMPLETE BACKEND TEST SUITE")
        print("=" * 60)
        
        # Test 1: Health check
        print("\n1. Health Check")
        r = await client.get(f"{BASE_URL}/health")
        print(f"   Status: {r.status_code}")
        print(f"   Response: {r.json()}")
        
        # Test 2: Create user with validation
        print("\n2. Create User (Pydantic Validation)")
        user_data = {
            "email": "test@example.com",
            "username": "testuser123",
            "password": "securepass123"
        }
        r = await client.post(f"{BASE_URL}/users", json=user_data)
        print(f"   Status: {r.status_code}")
        user = r.json()
        print(f"   Created: {user}")
        user_id = user["id"]
        
        # Test 3: Validation error
        print("\n3. Validation Error (short password)")
        bad_user = {
            "email": "bad@example.com",
            "username": "bad",
            "password": "123"
        }
        r = await client.post(f"{BASE_URL}/users", json=bad_user)
        print(f"   Status: {r.status_code}")
        print(f"   Error: {r.json()}")
        
        # Test 4: List users (pagination)
        print("\n4. List Users (Pagination)")
        r = await client.get(f"{BASE_URL}/users?skip=0&limit=5")
        print(f"   Status: {r.status_code}")
        print(f"   Users: {len(r.json())} found")
        
        # Test 5: Get user by ID
        print("\n5. Get User by ID")
        r = await client.get(f"{BASE_URL}/users/{user_id}")
        print(f"   Status: {r.status_code}")
        print(f"   User: {r.json()}")
        
        # Test 6: Protected route without auth
        print("\n6. Protected Route (No Auth)")
        r = await client.get(f"{BASE_URL}/protected")
        print(f"   Status: {r.status_code}")
        print(f"   Error: {r.json()}")
        
        # Test 7: Protected route with auth
        print("\n7. Protected Route (With Auth)")
        headers = {"Authorization": f"Bearer {user_id}"}
        r = await client.get(f"{BASE_URL}/protected", headers=headers)
        print(f"   Status: {r.status_code}")
        print(f"   Response: {r.json()}")
        
        # Test 8: Create post (protected)
        print("\n8. Create Post (Protected Route)")
        post_data = {
            "title": "My First Post",
            "content": "This is the content of my first post!"
        }
        r = await client.post(f"{BASE_URL}/posts", json=post_data, headers=headers)
        print(f"   Status: {r.status_code}")
        post = r.json()
        print(f"   Created: {post}")
        post_id = post["id"]
        
        # Test 9: List posts
        print("\n9. List Posts")
        r = await client.get(f"{BASE_URL}/posts")
        print(f"   Status: {r.status_code}")
        print(f"   Posts: {len(r.json())} found")
        
        # Test 10: Get post by ID
        print("\n10. Get Post by ID")
        r = await client.get(f"{BASE_URL}/posts/{post_id}")
        print(f"   Status: {r.status_code}")
        print(f"   Post: {r.json()}")
        
        # Test 11: Delete post (protected)
        print("\n11. Delete Post (Protected)")
        r = await client.delete(f"{BASE_URL}/posts/{post_id}", headers=headers)
        print(f"   Status: {r.status_code}")
        
        # Test 12: Verify deletion
        print("\n12. Verify Post Deleted")
        r = await client.get(f"{BASE_URL}/posts/{post_id}")
        print(f"   Status: {r.status_code}")
        print(f"   Error: {r.json()}")
        
        # Test 13: Create another user
        print("\n13. Create Second User")
        user2_data = {
            "email": "user2@example.com",
            "username": "user2test",
            "password": "password123"
        }
        r = await client.post(f"{BASE_URL}/users", json=user2_data)
        print(f"   Status: {r.status_code}")
        user2 = r.json()
        print(f"   Created: {user2}")
        
        # Test 14: Duplicate email error
        print("\n14. Duplicate Email Error")
        r = await client.post(f"{BASE_URL}/users", json=user_data)
        print(f"   Status: {r.status_code}")
        print(f"   Error: {r.json()}")
        
        # Test 15: CORS middleware
        print("\n15. CORS Middleware Test")
        r = await client.options(f"{BASE_URL}/users")
        print(f"   Status: {r.status_code}")
        print(f"   CORS Headers: {dict(r.headers)}")
        
        print("\n" + "=" * 60)
        print("ALL TESTS COMPLETED")
        print("=" * 60)

if __name__ == "__main__":
    asyncio.run(test_all())
