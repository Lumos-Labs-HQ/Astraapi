# FastAPI-Rust Test Failures - Prioritized Fix List

## Test Results Summary
- **Total Tests Run**: ~200
- **Passed**: 63
- **Failed**: 133
- **Errors**: 2

---

## PRIORITY 1 - CRITICAL (Core Functionality Broken)

### 1. Validation Error Handling (500 instead of 422)
**Impact**: HIGH - Breaks basic validation
**Tests Affected**: 
- `test_annotated.py::test_get` (5 failures)
- Multiple validation tests returning 500 instead of 422

**Issue**: Required query parameters cause 500 Internal Server Error instead of 422 Validation Error
**Root Cause**: Exception handling in C++ core or validation pipeline
**Fix Location**: `cpp_core/src/request_pipeline.cpp` or `fastapi/_cpp_server.py`

---

### 2. Dependency Caching Not Working
**Impact**: HIGH - Breaks dependency injection optimization
**Tests Affected**:
- `test_dependency_cache.py::test_sub_counter_no_cache`
- `test_dependency_cache.py::test_security_cache`

**Issue**: Dependencies called multiple times when `use_cache=False` should call once per request
**Root Cause**: C++ dependency engine not respecting cache settings
**Fix Location**: `cpp_core/src/dep_engine.cpp`

---

### 3. Generator Dependencies (After Yield)
**Impact**: HIGH - Breaks context managers and cleanup
**Tests Affected**:
- `test_dependency_after_yield_raise.py` (4 failures)
- `test_dependency_after_yield_streaming.py` (7 failures)
- `test_dependency_after_yield_websockets.py` (2 failures)

**Issue**: Dependencies with `yield` not executing cleanup code
**Root Cause**: C++ doesn't handle generator-based dependencies
**Fix Location**: `cpp_core/src/dep_engine.cpp` + `fastapi/dependencies/utils.py`

---

## PRIORITY 2 - IMPORTANT (Features Not Working)

### 4. OpenAPI Schema Generation Differences
**Impact**: MEDIUM - Documentation incorrect but app works
**Tests Affected**:
- `test_additional_properties_bool.py::test_openapi_schema`
- `test_additional_responses_*.py` (multiple)
- `test_application.py::test_openapi_schema`

**Issue**: Generated OpenAPI schema differs from expected (minor field ordering/structure)
**Root Cause**: C++ OpenAPI generator produces slightly different output
**Fix Location**: `cpp_core/src/openapi_gen.cpp`

---

### 5. Swagger UI / ReDoc HTML Generation
**Impact**: MEDIUM - UI docs don't load properly
**Tests Affected**:
- `test_application.py::test_swagger_ui`
- `test_application.py::test_swagger_ui_oauth2_redirect`
- `test_application.py::test_redoc`

**Issue**: HTML templates missing version strings or OAuth redirect URLs
**Root Cause**: Template generation in Python layer
**Fix Location**: `fastapi/openapi/docs.py`

---

### 6. Class-Based Dependencies
**Impact**: MEDIUM - OOP dependency pattern broken
**Tests Affected**:
- `test_dependency_class.py` (multiple)
- `test_dependency_wrapped.py`

**Issue**: Callable classes as dependencies not working
**Root Cause**: C++ doesn't recognize `__call__` method
**Fix Location**: `cpp_core/src/dep_engine.cpp`

---

## PRIORITY 3 - LOW (Edge Cases / Compatibility)

### 7. Starlette Compatibility
**Impact**: LOW - Only affects mixed usage
**Tests Affected**:
- `test_http_connection_injection.py` (FIXED)
- `test_tutorial/test_graphql/` (GraphQL router)

**Issue**: Third-party libraries using Starlette types
**Status**: Partially fixed - need to handle more Starlette types
**Fix Location**: `fastapi/dependencies/utils.py`

---

### 8. Behind Proxy / Root Path
**Impact**: LOW - Deployment-specific
**Tests Affected**:
- `test_tutorial/test_behind_a_proxy/`

**Issue**: TestClient doesn't support `root_path` parameter
**Fix Location**: `fastapi/_testclient.py`

---

## Recommended Fix Order

1. **Fix validation error handling** (Priority 1.1) - 1-2 hours
   - Add proper exception catching in C++ request pipeline
   - Return 422 with validation errors instead of 500

2. **Fix dependency caching** (Priority 1.2) - 2-3 hours
   - Implement cache flag checking in C++ dep_engine
   - Add per-request cache storage

3. **Implement generator dependency support** (Priority 1.3) - 4-6 hours
   - Detect generator functions in Python
   - Call cleanup code after response sent
   - Handle exceptions in cleanup

4. **Fix OpenAPI schema generation** (Priority 2.4) - 2-3 hours
   - Match field ordering with Python implementation
   - Fix schema references

5. **Fix Swagger UI templates** (Priority 2.5) - 1 hour
   - Update HTML templates with correct CDN versions

6. **Add class-based dependency support** (Priority 2.6) - 2-3 hours
   - Check for `__call__` method in C++
   - Handle callable objects

---

## Quick Wins (Can Fix Immediately)

1. ✅ **WebSocket with Depends** - FIXED
2. **Swagger UI version strings** - 30 minutes
3. **TestClient root_path parameter** - 30 minutes

---

## Total Estimated Effort
- **Critical Fixes**: 7-11 hours
- **Important Fixes**: 5-7 hours  
- **Low Priority**: 2-3 hours
- **Total**: 14-21 hours

---

## Current Status
✅ Core HTTP routing works
✅ Basic endpoints work  
✅ JSON serialization works
✅ WebSocket connections work
❌ Validation error handling broken
❌ Dependency caching broken
❌ Generator dependencies not supported
⚠️  OpenAPI schema has minor differences
