#!/usr/bin/env bash
# Build and run CI simulation Docker containers.
#
# Usage:
#   ./test/ci-sim/run.sh                # linux-amd64 only
#   ./test/ci-sim/run.sh --musl        # linux-amd64-musl only
#   ./test/ci-sim/run.sh --all         # both
#   ./test/ci-sim/run.sh --no-cache    # fresh build
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_AMD64="yaddnsc-ci"
IMAGE_MUSL="yaddnsc-ci-musl"

# Parse flags
BUILD_ARGS=()
TARGETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --musl) TARGETS+=("musl") ;;
        --all)  TARGETS+=("amd64" "musl") ;;
        --no-cache) BUILD_ARGS+=("--no-cache") ;;
        *) BUILD_ARGS+=("$1") ;;
    esac
    shift
done

# Default: linux-amd64 only
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS+=("amd64")
fi

for target in "${TARGETS[@]}"; do
    case "$target" in
        amd64)
            echo "=== Building linux-amd64 image ==="
            docker build "${SCRIPT_DIR}/../.." -f "${SCRIPT_DIR}/Dockerfile" \
                -t "${IMAGE_AMD64}" "${BUILD_ARGS[@]}"
            echo ""
            echo "=== Running linux-amd64 ==="
            docker run --rm "${IMAGE_AMD64}"
            ;;
        musl)
            echo "=== Building linux-amd64-musl image ==="
            docker build "${SCRIPT_DIR}/../.." -f "${SCRIPT_DIR}/Dockerfile.musl" \
                -t "${IMAGE_MUSL}" "${BUILD_ARGS[@]}"
            echo ""
            echo "=== Running linux-amd64-musl ==="
            docker run --rm "${IMAGE_MUSL}"
            ;;
    esac
done
