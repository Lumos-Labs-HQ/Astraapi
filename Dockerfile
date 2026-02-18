# Build stage
FROM python:3.14-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    make \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY cpp_core/ ./cpp_core/
COPY fastapi/ ./fastapi/
COPY pyproject.toml LICENSE README.md ./

# Build C++ extension
WORKDIR /build/cpp_core
RUN rm -rf build && \
    mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc) && \
    cp _fastapi_core.so ../../fastapi/

WORKDIR /build
RUN pip install --no-cache-dir --user ".[standard]"

# Runtime stage
FROM python:3.14-slim

WORKDIR /app

COPY --from=builder /root/.local /root/.local
COPY --from=builder /build/fastapi /app/fastapi
COPY test.py /app/

ENV PATH=/root/.local/bin:$PATH

CMD ["python", "test.py", "--host=0.0.0.0", "--port=8002"]
