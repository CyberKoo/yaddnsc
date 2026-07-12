#!/usr/bin/env bash
# Build yaddnsc .deb packages for one or more Ubuntu versions.
#
# Usage:
#   ./docker/build-deb.sh                                # builds for 24.04, defaults
#   ./docker/build-deb.sh 24.04                          # single version
#   ./docker/build-deb.sh 24.04 26.04                    # multiple versions
#   ./docker/build-deb.sh 24.04 --native-dns             # enable native DNS
#   ./docker/build-deb.sh 24.04 --no-system-spdlog       # use bundled spdlog
#   ./docker/build-deb.sh 24.04 --dns-server 8.8.8.8     # override default DNS server
#   ./docker/build-deb.sh 24.04 --dns-port 5353          # override default DNS port
#   ./docker/build-deb.sh 24.04 --extra "-DSOME_VAR=ON"  # any other cmake option
#
# Output:  ./deb-out/<ubuntu-version>/yaddnsc_<ver>_<arch>.deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/deb-out"

# Parse options
CMAKE_EXTRA_ARGS=""
POSITIONAL=()
while [ $# -gt 0 ]; do
    case "$1" in
        --native-dns)
            YADDNSC_USE_NATIVE_DNS=ON
            shift
            ;;
        --no-system-spdlog)
            YADDNSC_USE_SYSTEM_SPDLOG=OFF
            shift
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

# Defaults
YADDNSC_USE_NATIVE_DNS=${YADDNSC_USE_NATIVE_DNS:-OFF}
YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG:-ON}
YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER:-1.1.1.1}
YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT:-53}

# Default to 24.04 if no arguments
if [ $# -eq 0 ]; then
    set -- 24.04
fi

for UBUNTU_VERSION in "$@"; do
    VERSION_OUTPUT_DIR="${OUTPUT_DIR}/${UBUNTU_VERSION}"
    mkdir -p "${VERSION_OUTPUT_DIR}"

    echo "=========================================="
    echo " Building for Ubuntu ${UBUNTU_VERSION}..."
    echo " Options: YADDNSC_USE_NATIVE_DNS=${YADDNSC_USE_NATIVE_DNS}, YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG}"
    echo " Default DNS: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}"
    [[ -n "${CMAKE_EXTRA_ARGS}" ]] && echo " Extra: ${CMAKE_EXTRA_ARGS}"
    echo "=========================================="

    docker build -f "${SCRIPT_DIR}/docker/Dockerfile.ubuntu" \
        --build-arg "UBUNTU_VERSION=${UBUNTU_VERSION}" \
        --build-arg "YADDNSC_USE_NATIVE_DNS=${YADDNSC_USE_NATIVE_DNS}" \
        --build-arg "YADDNSC_USE_SYSTEM_SPDLOG=${YADDNSC_USE_SYSTEM_SPDLOG}" \
        --build-arg "YADDNSC_DEFAULT_DNS_SERVER=${YADDNSC_DEFAULT_DNS_SERVER}" \
        --build-arg "YADDNSC_DEFAULT_DNS_PORT=${YADDNSC_DEFAULT_DNS_PORT}" \
        --build-arg "CMAKE_EXTRA_ARGS=${CMAKE_EXTRA_ARGS}" \
        -o "${VERSION_OUTPUT_DIR}" \
        "${SCRIPT_DIR}"

    echo "→ Output: ${VERSION_OUTPUT_DIR}/"
    ls -lh "${VERSION_OUTPUT_DIR}"/yaddnsc_*.deb 2>/dev/null || true
done
