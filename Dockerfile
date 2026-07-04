# =============================================================================
# Stage 1: Build
# =============================================================================
FROM alpine:3 AS builder

# Install toolchain and system dependencies
RUN apk add --no-cache \
        build-base \
        cmake \
        git \
        openssl-dev \
        zlib-dev \
        brotli-dev

WORKDIR /build

# Copy entire project source
COPY . .

# Create build directory and configure
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_SYSCONFDIR=/etc

RUN cmake --build build -j "$(nproc)"

# Install to staging prefix
RUN DESTDIR=/install cmake --install build

# =============================================================================
# Stage 2: Runtime
# =============================================================================
FROM alpine:3 AS runtime

# Install only runtime libraries
RUN apk add --no-cache \
        openssl \
        libstdc++ \
        zlib \
        brotli \
        ca-certificates

# Copy the compiled binary, config, and driver modules
COPY --from=builder /install /

# Create a non-root user for running the service
RUN addgroup -S yaddnsc && adduser -S -G yaddnsc yaddnsc

WORKDIR /etc/yaddnsc

USER yaddnsc

ENTRYPOINT ["yaddnsc"]
CMD ["run", "-c", "/etc/yaddnsc/config.json"]
