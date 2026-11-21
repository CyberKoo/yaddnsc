#!/usr/bin/env bash
# Build and install-test yaddnsc RPM packages in Fedora or openSUSE containers.
#
# Usage:
#   ./docker/build-rpm.sh
#   ./docker/build-rpm.sh fedora:43
#   ./docker/build-rpm.sh fedora:43 opensuse/tumbleweed:latest
#   ./docker/build-rpm.sh opensuse/leap:16.0 --release 2
#   ./docker/build-rpm.sh fedora:43 --jobs 2
#   ./docker/build-rpm.sh fedora:43 --dns-server 8.8.8.8 --dns-port 5353
#   ./docker/build-rpm.sh fedora:43 --extra "-DSOME_VAR=ON"
#
# Output: ./rpm-out/<image-name>/yaddnsc-*.rpm
#         ./rpm-out/<image-name>/yaddnsc-devel-*.rpm

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/rpm-out"
CMAKE_EXTRA_ARGS=""
RPM_RELEASE=1
BUILD_JOBS=2
POSITIONAL=()

while [ $# -gt 0 ]; do
    case "$1" in
        --release)
            RPM_RELEASE="$2"
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
    set -- fedora:43
fi

for BASE_IMAGE in "$@"; do
    OUTPUT_NAME="${BASE_IMAGE//\//_}"
    OUTPUT_NAME="${OUTPUT_NAME//:/_}"
    VERSION_OUTPUT_DIR="${OUTPUT_DIR}/${OUTPUT_NAME}"
    mkdir -p "${VERSION_OUTPUT_DIR}"

    echo "=========================================="
    echo " Building and verifying RPMs on ${BASE_IMAGE}..."
    echo " RPM release: ${RPM_RELEASE}"
    echo " Build jobs: ${BUILD_JOBS}"
    echo " Default DNS: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}"
    [[ -n "${CMAKE_EXTRA_ARGS}" ]] && echo " Extra: ${CMAKE_EXTRA_ARGS}"
    echo "=========================================="

    docker build -f "${SCRIPT_DIR}/docker/Dockerfile.rpm" \
        --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
        --build-arg "YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER}" \
        --build-arg "YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT}" \
        --build-arg "YADDNSC_RPM_PACKAGE_RELEASE=${RPM_RELEASE}" \
        --build-arg "BUILD_JOBS=${BUILD_JOBS}" \
        --build-arg "CMAKE_EXTRA_ARGS=${CMAKE_EXTRA_ARGS}" \
        -o "${VERSION_OUTPUT_DIR}" \
        "${SCRIPT_DIR}"

    echo "→ Output: ${VERSION_OUTPUT_DIR}/"
    ls -lh "${VERSION_OUTPUT_DIR}"/*.rpm
done
