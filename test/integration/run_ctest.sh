#!/usr/bin/env bash
# CTest integration test runner.
#
# Called from CTest with:
#   run_ctest.sh <PROJECT_DIR> <BUILD_DIR>
#
# Assumes yaddnsc and simple driver are already built by CMake.
# Manages its own venv, certs, and sim server lifecycle.
# ==============================================================================

set -euo pipefail

PROJECT_DIR="$1"
BUILD_DIR="$2"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

CERT_DIR="/tmp/sim-certs"
CERT_FILE="${CERT_DIR}/sim.crt"
VENV_DIR="/tmp/sim-venv"

PASS=true

cleanup() {
    echo "--- Cleaning up ---"
    [ -n "${SIM_PID:-}" ] && kill "${SIM_PID}" 2>/dev/null || true
    wait "${SIM_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== yaddnsc Integration Test (CTest) ==="
echo "  Project: ${PROJECT_DIR}"
echo "  Build:   ${BUILD_DIR}"
echo ""

# ---------------------------------------------------------------------------
# 0. Ensure system dependencies (curl, openssl)
# ---------------------------------------------------------------------------
if ! command -v curl &>/dev/null; then
    echo "--- Installing curl ---"
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq && sudo apt-get install -y -qq curl
    elif command -v apk &>/dev/null; then
        apk add --no-cache curl
    else
        echo "ERROR: curl not found and cannot be auto-installed"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# 1. Verify binaries exist
# ---------------------------------------------------------------------------
YADDNSC_BIN="${BUILD_DIR}/yaddnsc"
DRIVER_DIR="${BUILD_DIR}/driver/simple"

if [ ! -f "${YADDNSC_BIN}" ]; then
    echo "ERROR: yaddnsc binary not found at ${YADDNSC_BIN}"
    echo "Build the project first: ninja -C ${BUILD_DIR} yaddnsc simple"
    exit 1
fi

SIMPLE_SO="${DRIVER_DIR}/simple.so"
if [ ! -f "${SIMPLE_SO}" ]; then
    # Try alternative locations
    SIMPLE_SO="${BUILD_DIR}/simple.so"
    if [ ! -f "${SIMPLE_SO}" ]; then
        echo "ERROR: simple driver not found"
        echo "Build it first: ninja -C ${BUILD_DIR} simple"
        exit 1
    fi
fi

DRIVER_DIR="$(dirname "${SIMPLE_SO}")"
echo "  yaddnsc:    ${YADDNSC_BIN}"
echo "  driver dir: ${DRIVER_DIR}"

# ---------------------------------------------------------------------------
# 2. Python venv + dependencies (cached)
# ---------------------------------------------------------------------------
# Clean up stale/incomplete venv from a previous failed run.
if [ -d "${VENV_DIR}" ] && [ ! -f "${VENV_DIR}/bin/activate" ]; then
    rm -rf "${VENV_DIR}"
fi

if [ ! -d "${VENV_DIR}" ]; then
    echo "--- Setting up Python venv ---"
    # Need python3-venv (ensurepip) to bootstrap pip inside the venv.
    # System pip on modern Debian/Ubuntu refuses to install packages (PEP 668).
    python3 -m venv "${VENV_DIR}" 2>/dev/null || {
        echo ""
        echo "SKIPPED: python3 -m venv failed (ensurepip not available)."
        echo "Install python3-venv: sudo apt install python3-venv"
        echo "Or use CI Docker:  test/ci-sim/run.sh"
        exit 77
    }
fi
source "${VENV_DIR}/bin/activate"

# Install Python dependencies from requirements.txt.
pip install -q -r "${SCRIPT_DIR}/requirements.txt"

# ---------------------------------------------------------------------------
# 3. Generate self-signed TLS cert (cached)
# ---------------------------------------------------------------------------
if [ ! -f "${CERT_FILE}" ]; then
    echo "--- Generating TLS certificate ---"
    mkdir -p "${CERT_DIR}"
    openssl req -x509 -newkey rsa:2048 -keyout "${CERT_DIR}/sim.key" \
        -out "${CERT_FILE}" -days 365 -nodes \
        -subj "/CN=sim/O=yaddnsc/C=XX" \
        -addext "subjectAltName=DNS:sim,DNS:localhost,IP:127.0.0.1" 2>&1 | grep -v "^[.+*]"
fi

cp "${CERT_FILE}" "${PROJECT_DIR}/ca.pem"
export SSL_CERT_FILE="${PROJECT_DIR}/ca.pem"
echo "  Certificate: ${CERT_FILE} -> ${PROJECT_DIR}/ca.pem"

# ---------------------------------------------------------------------------
# 4. Start sim server
# ---------------------------------------------------------------------------
echo "--- Starting sim server ---"
export SIM_DNS_PORT=${SIM_DNS_PORT:-15353}
export SIM_DOT_PORT=${SIM_DOT_PORT:-1853}
export SIM_DOH_PORT=${SIM_DOH_PORT:-1443}
export SIM_API_PORT=${SIM_API_PORT:-8080}
export SIM_CERT_DIR="${CERT_DIR}"

python3 -u "${SCRIPT_DIR}/sim/server.py" &
SIM_PID=$!

echo "--- Waiting for sim server ---"
for i in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:${SIM_API_PORT}/health" > /dev/null 2>&1; then
        echo "  sim ready after ${i}s"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "ERROR: sim failed to start"
        exit 1
    fi
    sleep 1
done

# ---------------------------------------------------------------------------
# 5. Run test scenarios
# ---------------------------------------------------------------------------
run_scenario() {
    local name="$1"
    local config="$2"
    local expect_ip="$3"
    local expect_tag="$4"
    local resolver_ok_pattern="${5:-}"

    echo ""
    echo "=== Scenario: ${name} ==="

    local tmpfile yaddnsc_out
    tmpfile=$(mktemp /tmp/yaddnsc-test-XXXXXX.json)
    yaddnsc_out=$(mktemp /tmp/yaddnsc-output-XXXXXX.txt)
    sed "s|__DRIVER_DIR__|${DRIVER_DIR}|g" "${config}" > "${tmpfile}"

    # Reset sim logs
    curl -sf "http://127.0.0.1:${SIM_API_PORT:-8080}/reset" > /dev/null 2>&1 || true

    # Run yaddnsc (capture all output for resolver verification)
    "${YADDNSC_BIN}" run -c "${tmpfile}" -d > "${yaddnsc_out}" 2>&1 &
    local pid=$!
    sleep 2
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    echo "  yaddnsc exited"

    # Fetch logs
    local logs
    logs=$(curl -sf "http://127.0.0.1:${SIM_API_PORT}/logs")
    echo "  Logs: ${logs}"

    # Verify update request
    local update_ok=true
    if ! echo "${logs}" | python3 -c "
import sys, json
logs = json.load(sys.stdin)
for req in logs:
    if '${expect_tag}' in req.get('path', '') and 'ip=${expect_ip}' in req.get('path', ''):
        sys.exit(0)
print('FAIL: no match for tag=${expect_tag} ip=${expect_ip}', file=sys.stderr)
sys.exit(1)
"; then
        update_ok=false
    fi

    # Verify resolver actually succeeded (if pattern provided)
    local resolver_ok=true
    if [ -n "${resolver_ok_pattern}" ]; then
        if ! grep -q "${resolver_ok_pattern}" "${yaddnsc_out}" 2>/dev/null; then
            resolver_ok=false
            echo "  RESOLVER FAIL: pattern '${resolver_ok_pattern}' not found in yaddnsc output"
            # Print relevant lines for debugging
            grep -i "resolver\|query\|tls\|connection" "${yaddnsc_out}" 2>/dev/null | sed 's/^/    | /'
        fi
    fi

    if [ "${update_ok}" = true ] && [ "${resolver_ok}" = true ]; then
        echo "  ✓ ${name} PASS"
    else
        echo "  ✗ ${name} FAIL"
        PASS=false
    fi

    rm -f "${tmpfile}" "${yaddnsc_out}"
}

# === INTERFACE source + Classic resolver ===
run_scenario \
    "iface+classic" \
    "${SCRIPT_DIR}/configs/config.classic.json" \
    "127.0.0.1" \
    "source=iface&resolver=classic" \
    "Resolving.*iface.yaddnsc.test"

# === HTTP source + DoT resolver ===
run_scenario \
    "http+dot" \
    "${SCRIPT_DIR}/configs/config.dot.json" \
    "198.51.100.1" \
    "source=http&resolver=dot" \
    "query succeeded.*http.yaddnsc.test"

# === INTERFACE source + DoH resolver ===
run_scenario \
    "iface+doh" \
    "${SCRIPT_DIR}/configs/config.doh.iface.json" \
    "127.0.0.1" \
    "source=iface&resolver=doh" \
    "query succeeded.*iface.yaddnsc.test"

# ---------------------------------------------------------------------------
# 6. Final result
# ---------------------------------------------------------------------------
echo ""
if [ "${PASS}" = true ]; then
    echo "=== ALL SCENARIOS PASSED ==="
    exit 0
else
    echo "=== SOME SCENARIOS FAILED ==="
    exit 1
fi
