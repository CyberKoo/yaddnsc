#!/usr/bin/env bash
# Build yaddnsc DEB packages in Debian or Ubuntu containers.
#
# Usage:
#   ./docker/build-deb.sh                                # Ubuntu 24.04
#   ./docker/build-deb.sh ubuntu:24.04
#   ./docker/build-deb.sh ubuntu:24.04 ubuntu:26.04      # sequential builds
#   ./docker/build-deb.sh debian:13
#   ./docker/build-deb.sh ubuntu:24.04 debian:13         # sequential builds
#   ./docker/build-deb.sh debian:13 --jobs 2             # limit compile jobs
#   ./docker/build-deb.sh ubuntu:24.04 --no-system-spdlog
#   ./docker/build-deb.sh ubuntu:24.04 --dns-server 8.8.8.8
#   ./docker/build-deb.sh ubuntu:24.04 --dns-port 5353
#   ./docker/build-deb.sh ubuntu:24.04 --extra "-DSOME_VAR=ON"
#
# Targets must use ubuntu:<tag> or debian:<tag>. Other distributions and Ubuntu
# version-only shorthand are rejected before any build starts.
#
# Output: ./deb-out/<image-name>/yaddnsc_*.deb
#         ./deb-out/<image-name>/yaddnsc-dev_*.deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/deb-out"
CMAKE_EXTRA_ARGS=""
BUILD_JOBS=2
POSITIONAL=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-system-spdlog)
            YADDNSC_USE_SYSTEM_SPDLOG=OFF
            shift
            ;;
        --jobs)
            BUILD_JOBS="$2"
            shift 2
            ;;
        --dns-server)
            YADDNSC_DEFAULT_DNS_SERVER="$2"
            shift 2
            ;;
        --dns-port)
            YADDNSC_DEFAULT_DNS_PORT="$2"
            shift 2
            ;;
        --extra)
            CMAKE_EXTRA_ARGS="$2"
            shift 2
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

set -- "${POSITIONAL[@]}"
YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG:-ON}
YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER:-1.1.1.1}
YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT:-53}

if ! [[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi

if [ $# -eq 0 ]; then
    set -- ubuntu:24.04
fi

for TARGET in "$@"; do
    if ! [[ "${TARGET}" =~ ^(ubuntu|debian):[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
        echo "Unsupported DEB target '${TARGET}'. Use ubuntu:<tag> or debian:<tag>." >&2
        exit 2
    fi
done

for BASE_IMAGE in "$@"; do
    OUTPUT_NAME="${BASE_IMAGE//\//_}"
    OUTPUT_NAME="${OUTPUT_NAME//:/_}"
    VERSION_OUTPUT_DIR="${OUTPUT_DIR}/${OUTPUT_NAME}"
    mkdir -p "${VERSION_OUTPUT_DIR}"

    echo "=========================================="
    echo " Building DEBs on ${BASE_IMAGE}..."
    echo " Options: YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG}"
    echo " Build jobs: ${BUILD_JOBS}"
    echo " Default DNS: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}"
    [[ -n "${CMAKE_EXTRA_ARGS}" ]] && echo " Extra: ${CMAKE_EXTRA_ARGS}"
    echo "=========================================="

    docker build -f "${SCRIPT_DIR}/docker/Dockerfile.deb" \
        --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
        --build-arg "YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG}" \
        --build-arg "YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER}" \
        --build-arg "YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT}" \
        --build-arg "BUILD_JOBS=${BUILD_JOBS}" \
        --build-arg "CMAKE_EXTRA_ARGS=${CMAKE_EXTRA_ARGS}" \
        -o "${VERSION_OUTPUT_DIR}" \
        "${SCRIPT_DIR}"

    echo "→ Output: ${VERSION_OUTPUT_DIR}/"
    ls -lh "${VERSION_OUTPUT_DIR}"/*.deb
done
