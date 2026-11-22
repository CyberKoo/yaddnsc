#!/usr/bin/env bash
# Build and install-test yaddnsc APK packages in Alpine containers.
#
# Usage:
#   ./docker/build-apk.sh
#   ./docker/build-apk.sh alpine:3
#   ./docker/build-apk.sh alpine:3.24 --jobs 2
#   ./docker/build-apk.sh alpine:3 --release 1
#   ./docker/build-apk.sh alpine:3 --dns-server 8.8.8.8 --dns-port 5353
#   ./docker/build-apk.sh alpine:3 --extra "-DSOME_VAR=ON"
#
# Targets must use alpine:<tag>. Other distributions are rejected before building.
#
# Output: ./apk-out/<image-name>/yaddnsc-*.apk
#         ./apk-out/<image-name>/yaddnsc-dev-*.apk
#         ./apk-out/<image-name>/yaddnsc-openrc-*.apk

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/apk-out"
CMAKE_EXTRA_ARGS=""
APK_RELEASE=0
BUILD_JOBS=2
POSITIONAL=()

while [ $# -gt 0 ]; do
    case "$1" in
        --release)
            APK_RELEASE="$2"
            shift 2
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
YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER:-1.1.1.1}
YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT:-53}

if ! [[ "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi

if [ $# -eq 0 ]; then
    set -- alpine:3
fi

for TARGET in "$@"; do
    if ! [[ "${TARGET}" =~ ^alpine:[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
        echo "Unsupported APK target '${TARGET}'. Use alpine:<tag>." >&2
        exit 2
    fi
done

for BASE_IMAGE in "$@"; do
    OUTPUT_NAME="${BASE_IMAGE//\//_}"
    OUTPUT_NAME="${OUTPUT_NAME//:/_}"
    VERSION_OUTPUT_DIR="${OUTPUT_DIR}/${OUTPUT_NAME}"
    mkdir -p "${VERSION_OUTPUT_DIR}"

    echo "=========================================="
    echo " Building and verifying APKs on ${BASE_IMAGE}..."
    echo " APK release: ${APK_RELEASE}"
    echo " Build jobs: ${BUILD_JOBS}"
    echo " Default DNS: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}"
    [[ -n "${CMAKE_EXTRA_ARGS}" ]] && echo " Extra: ${CMAKE_EXTRA_ARGS}"
    echo "=========================================="

    docker build -f "${SCRIPT_DIR}/docker/Dockerfile.apk" \
        --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
        --build-arg "YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER}" \
        --build-arg "YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT}" \
        --build-arg "YADDNSC_APK_PACKAGE_RELEASE=${APK_RELEASE}" \
        --build-arg "BUILD_JOBS=${BUILD_JOBS}" \
        --build-arg "CMAKE_EXTRA_ARGS=${CMAKE_EXTRA_ARGS}" \
        -o "${VERSION_OUTPUT_DIR}" \
        "${SCRIPT_DIR}"

    echo "→ Output: ${VERSION_OUTPUT_DIR}/"
    ls -lh "${VERSION_OUTPUT_DIR}"/*.apk
done
