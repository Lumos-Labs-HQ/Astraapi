cmake_minimum_required(VERSION 3.20)
project(_fastapi_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Find Python ──────────────────────────────────────────────────────────────
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

# ── Sources ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/module.cpp
    src/asgi_constants.cpp
    src/buffer_pool.cpp
    src/json_writer.cpp
    src/utils.cpp
    src/router.cpp
    src/http_parser.cpp
    src/app.cpp
    src/body_parser.cpp
    src/middleware_engine.cpp
    src/form_parser.cpp
    src/json_encoder.cpp
    src/param_extractor.cpp
    src/openapi_gen.cpp
    src/dep_engine.cpp
    src/security.cpp
    src/websocket_handler.cpp
    src/error_response.cpp
    src/request_pipeline.cpp
    src/response_pipeline.cpp
    src/dependency_resolver.cpp
    src/json_parser.cpp
    src/ws_frame_parser.cpp
    src/ws_ring_buffer.cpp
)

# ── Third-party C libraries (yyjson, ryu, llhttp) ─────────────────────────
set(THIRD_PARTY_C_SOURCES
    third_party/yyjson/yyjson.c
    third_party/ryu/d2s.c
    third_party/llhttp/llhttp.c
    third_party/llhttp/api.c
    third_party/llhttp/http.c
)
set_source_files_properties(${THIRD_PARTY_C_SOURCES} PROPERTIES LANGUAGE C)

# ── Build the extension module ───────────────────────────────────────────────
Python3_add_library(_fastapi_core MODULE ${SOURCES} ${THIRD_PARTY_C_SOURCES})

target_include_directories(_fastapi_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ryu
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llhttp
    ${Python3_INCLUDE_DIRS}
)

# ── PGO (Profile-Guided Optimization) ────────────────────────────────────────
# Usage (GCC/Clang):
#   Step 1: cmake -DENABLE_PGO_GENERATE=ON .. && make -j$(nproc)
#   Step 2: Run benchmarks to generate profile data
#   Step 3: cmake -DENABLE_PGO_USE=ON .. && make -j$(nproc)
option(ENABLE_PGO_GENERATE "Build with PGO instrumentation" OFF)
option(ENABLE_PGO_USE "Build with PGO data" OFF)

# ── Compiler flags: maximum optimization (platform-specific) ─────────────────
if(MSVC)
    # MSVC (Windows)
    target_compile_options(_fastapi_core PRIVATE
        /O2 /Ob3 /GL /fp:fast /arch:AVX2
        /W3 /wd4244 /wd4267 /wd4996
    )
    target_compile_options(_fastapi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    target_link_options(_fastapi_core PRIVATE /LTCG)

    # PGO for MSVC
    if(ENABLE_PGO_GENERATE)
        target_compile_options(_fastapi_core PRIVATE /GL)
        target_link_options(_fastapi_core PRIVATE /LTCG:PGI)
        message(STATUS "PGO: MSVC instrumentation enabled")
    elseif(ENABLE_PGO_USE)
        target_compile_options(_fastapi_core PRIVATE /GL)
        target_link_options(_fastapi_core PRIVATE /LTCG:PGO)
        message(STATUS "PGO: MSVC using profile data")
    endif()
else()
    # GCC / Clang (Linux, macOS)
    target_compile_options(_fastapi_core PRIVATE
        -O3
        -march=native
        -flto
        -fno-math-errno
        -funroll-loops
        -fomit-frame-pointer
        -ftree-vectorize
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type
    )
    target_compile_options(_fastapi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    target_compile_options(_fastapi_core PRIVATE -fvisibility=hidden)
    target_compile_options(_fastapi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)

    # -Wno-free-nonheap-object: suppress GCC 14 false positive in LTO-inlined vector::push_back
    target_link_options(_fastapi_core PRIVATE -flto -Wno-free-nonheap-object)

    # PGO for GCC/Clang
    if(ENABLE_PGO_GENERATE)
        target_compile_options(_fastapi_core PRIVATE -fprofile-generate)
        target_link_options(_fastapi_core PRIVATE -fprofile-generate)
        message(STATUS "PGO: Instrumentation enabled — run benchmarks to generate profile data")
    elseif(ENABLE_PGO_USE)
        target_compile_options(_fastapi_core PRIVATE -fprofile-use -fprofile-correction)
        target_link_options(_fastapi_core PRIVATE -fprofile-use)
        message(STATUS "PGO: Using profile data for optimization")
    endif()
endif()

# ── Link system libs ────────────────────────────────────────────────────────
# Try to find libdeflate (2-3x faster single-shot gzip); fall back to zlib
find_library(LIBDEFLATE deflate)
find_library(BROTLI_ENC brotlienc)
find_library(BROTLI_DEC brotlidec)

if(MSVC)
    find_package(ZLIB REQUIRED)
    target_link_libraries(_fastapi_core PRIVATE ZLIB::ZLIB ws2_32)
else()
    target_link_libraries(_fastapi_core PRIVATE z)
endif()

if(LIBDEFLATE)
    target_link_libraries(_fastapi_core PRIVATE ${LIBDEFLATE})
    target_compile_definitions(_fastapi_core PRIVATE HAS_LIBDEFLATE=1)
    message(STATUS "libdeflate found — using fast single-shot gzip compression")
else()
    message(STATUS "libdeflate not found — using zlib for gzip compression")
endif()

if(BROTLI_ENC AND BROTLI_DEC)
    target_link_libraries(_fastapi_core PRIVATE ${BROTLI_ENC} ${BROTLI_DEC})
    target_compile_definitions(_fastapi_core PRIVATE HAS_BROTLI=1)
else()
    message(STATUS "Brotli not found — compression will fall back to gzip only")
endif()

# ── yyjson optimization: strict JSON mode + SIMD (implied by -march=native) ──
target_compile_definitions(_fastapi_core PRIVATE
    YYJSON_DISABLE_NON_STANDARD=1
)

# ── Output name: match Python's expected module name ─────────────────────────
# Linux:   _fastapi_core.cpython-311-x86_64-linux-gnu.so
# Windows: _fastapi_core.pyd
set_target_properties(_fastapi_core PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
