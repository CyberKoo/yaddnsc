#!/usr/bin/env bash
# Build yaddnsc .deb packages for one or more Ubuntu versions.
#
# Usage:
#   ./docker/build-deb.sh                     # builds for 24.04
#   ./docker/build-deb.sh 24.04               # single version
#   ./docker/build-deb.sh 24.04 26.04         # multiple versions
#
# Output:  ./deb-out/<ubuntu-version>/yaddnsc_<ver>_<arch>.deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/deb-out"

# Default to 24.04 if no arguments
if [ $# -eq 0 ]; then
    set -- 24.04
fi

for UBUNTU_VERSION in "$@"; do
    VERSION_OUTPUT_DIR="${OUTPUT_DIR}/${UBUNTU_VERSION}"
    mkdir -p "${VERSION_OUTPUT_DIR}"

    echo "=========================================="
    echo " Building for Ubuntu ${UBUNTU_VERSION}..."
    echo "=========================================="

    docker build -f "${SCRIPT_DIR}/docker/Dockerfile.deb" \
        --build-arg "UBUNTU_VERSION=${UBUNTU_VERSION}" \
        -o "${VERSION_OUTPUT_DIR}" \
        "${SCRIPT_DIR}"

    echo "→ Output: ${VERSION_OUTPUT_DIR}/"
    ls -lh "${VERSION_OUTPUT_DIR}"/yaddnsc_*.deb 2>/dev/null || true
done
