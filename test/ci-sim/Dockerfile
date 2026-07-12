# yaddnsc CI simulation — linux-amd64 (GCC 14, Ubuntu 24.04)
#
# Usage:
#   docker build -f test/ci-sim/Dockerfile -t yaddnsc-ci .
#   docker run --rm yaddnsc-ci
# ==============================================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CC=gcc-14
ENV CXX=g++-14

# ── CI dependencies ──
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    gcc-14 g++-14 \
    libssl-dev \
    zlib1g-dev \
    pkg-config \
    python3-venv \
    git \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy source, excluding local build artifacts
COPY . .
RUN rm -rf build build-* build-coverage cmake-build-debug .cache

# ── [1/4] linux-amd64: build + unit/component tests ──
RUN echo "=== [1/4] linux-amd64 configure ===" \
    && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DYADDNSC_BUILD_TESTS=ON \
    && echo "=== [2/4] linux-amd64 build ===" \
    && ninja -C build -j$(nproc) \
    && echo "=== [3/4] linux-amd64 test ===" \
    && ctest --test-dir build --output-on-failure -j$(nproc)

# ── [2/4] conversion-gate: extra warning flags ──
RUN echo "=== [4/4] conversion-gate configure ===" \
    && cmake -B build-conversion -G Ninja -DCMAKE_BUILD_TYPE=Debug -DYADDNSC_BUILD_TESTS=ON \
        -DCMAKE_CXX_FLAGS="-Wconversion -Wsign-conversion" \
    && echo "=== [4/4] conversion-gate build ===" \
    && ninja -C build-conversion -j$(nproc) \
    && echo "=== [4/4] conversion-gate test ===" \
    && ctest --test-dir build-conversion --output-on-failure -j$(nproc)

# ── [3/4] benchmark: Release build + run ──
RUN echo "=== [5/4] benchmark build ===" \
    && ninja -C build -j$(nproc) bench_dns_parser_native bench_dns_parser_system \
        bench_uri bench_string_util bench_inet_address bench_socket_addr bench_builder \
    && echo "=== [5/4] benchmark results ===" \
    && for b in bench_dns_parser_native bench_dns_parser_system bench_uri \
               bench_string_util bench_inet_address bench_socket_addr bench_builder; do \
           echo "--- $b ---"; \
           ./build/test/perf/$b --benchmark_format=console 2>&1 | tail -n +12; \
       done

# ── [4/4] integration test: end-to-end scenarios ──
RUN echo "=== [6/4] integration test ===" \
    && ctest --test-dir build -R integration --output-on-failure

RUN echo "============================================" \
    && echo "  ALL CI STEPS PASSED (linux-amd64)" \
    && echo "============================================"
