cmake_minimum_required(VERSION 3.20)
project(_astraapi_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# scikit-build-core injects Python3 via Development.Module (works in all containers)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

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

set(THIRD_PARTY_C_SOURCES
    third_party/yyjson/yyjson.c
    third_party/ryu/d2s.c
    third_party/llhttp/llhttp.c
    third_party/llhttp/api.c
    third_party/llhttp/http.c
)
set_source_files_properties(${THIRD_PARTY_C_SOURCES} PROPERTIES LANGUAGE C)

Python3_add_library(_astraapi_core MODULE ${SOURCES} ${THIRD_PARTY_C_SOURCES})

target_include_directories(_astraapi_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ryu
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llhttp
    ${Python3_INCLUDE_DIRS}
)

if(MSVC)
    target_compile_options(_astraapi_core PRIVATE
        /O2 /GL /GS- /fp:fast /Oi /Gy
        /W3 /wd4100 /wd4127 /wd4201
    )
    target_link_options(_astraapi_core PRIVATE /LTCG)
else()
    # -march=native is only safe for native (non-cross) builds.
    # On macOS, cibuildwheel sets CMAKE_OSX_ARCHITECTURES when cross-compiling
    # (e.g. building x86_64 on arm64 runner). Skip -march=native in that case.
    if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(_MARCH "")
    else()
        set(_MARCH -march=native)
    endif()

    target_compile_options(_astraapi_core PRIVATE
        -O3 ${_MARCH} -flto -fno-math-errno -funroll-loops -fomit-frame-pointer -ftree-vectorize
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type
    )
    target_compile_options(_astraapi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    target_compile_options(_astraapi_core PRIVATE -fvisibility=hidden)
    target_compile_options(_astraapi_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
    target_link_options(_astraapi_core PRIVATE -flto -Wno-free-nonheap-object)
endif()

# On macOS, add Homebrew prefix to search paths (handles both Intel and Apple Silicon)
if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(BREW_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${BREW_PREFIX}")
        include_directories(SYSTEM "${BREW_PREFIX}/include")
        link_directories("${BREW_PREFIX}/lib")
    endif()
endif()

find_library(LIBDEFLATE deflate)
find_library(BROTLI_ENC brotlienc)
find_library(BROTLI_DEC brotlidec)

if(MSVC)
    target_link_libraries(_astraapi_core PRIVATE ws2_32)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(_astraapi_core PRIVATE ZLIB::ZLIB)
    endif()
else()
    target_link_libraries(_astraapi_core PRIVATE z)
endif()

if(LIBDEFLATE)
    target_link_libraries(_astraapi_core PRIVATE ${LIBDEFLATE})
    target_compile_definitions(_astraapi_core PRIVATE HAS_LIBDEFLATE=1)
    message(STATUS "libdeflate found")
endif()

if(BROTLI_ENC AND BROTLI_DEC)
    target_link_libraries(_astraapi_core PRIVATE ${BROTLI_ENC} ${BROTLI_DEC})
    target_compile_definitions(_astraapi_core PRIVATE HAS_BROTLI=1)
endif()

target_compile_definitions(_astraapi_core PRIVATE YYJSON_DISABLE_NON_STANDARD=1)

set_target_properties(_astraapi_core PROPERTIES
    PREFIX ""
)
