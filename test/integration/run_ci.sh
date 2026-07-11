#!/usr/bin/env bash
# CI-native integration test runner.
# Runs 3 test scenarios covering all IP sources and resolver types.
# No Docker required.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-integration"
CERT_DIR="/tmp/sim-certs"
CERT_FILE="${CERT_DIR}/sim.crt"

PASS=true

cleanup() {
    echo "--- Cleaning up ---"
    [ -n "${SIM_PID:-}" ] && kill "${SIM_PID}" 2>/dev/null || true
    wait "${SIM_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== yaddnsc CI Integration Test ==="
echo ""

# ---------------------------------------------------------------------------
# 1. Python venv + dependencies
# ---------------------------------------------------------------------------
echo "--- Installing Python dependencies ---"
python3 -m venv /tmp/sim-venv
source /tmp/sim-venv/bin/activate
pip install -q dnslib

# ---------------------------------------------------------------------------
# Generate self-signed TLS cert (reused by sim server and DoT/DoH tests)
mkdir -p "${CERT_DIR}"
openssl req -x509 -newkey rsa:2048 -keyout "${CERT_DIR}/sim.key" \
    -out "${CERT_FILE}" -days 365 -nodes \
    -subj "/CN=sim/O=yaddnsc/C=XX" \
    -addext "subjectAltName=DNS:sim,DNS:localhost,IP:127.0.0.1" 2>&1 | grep -v "^[.+*]"
echo "Certificate generated: ${CERT_FILE}"
echo "Certificate installed as ${BUILD_DIR}/ca.pem"

# ---------------------------------------------------------------------------
# 3. Start sim server
# ---------------------------------------------------------------------------
echo "--- Starting sim server ---"
export SIM_DNS_PORT=15353
export SIM_DOT_PORT=1853
export SIM_DOH_PORT=1443
export SIM_API_PORT=8080
export SIM_CERT_DIR="${CERT_DIR}"

python3 -u "${SCRIPT_DIR}/sim/server.py" &
SIM_PID=$!

echo "--- Waiting for sim server ---"
for i in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:${SIM_API_PORT}/health" > /dev/null 2>&1; then
        echo "sim ready after ${i}s"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "ERROR: sim failed to start"
        exit 1
    fi
    sleep 1
done

# ---------------------------------------------------------------------------
# 4. Build yaddnsc (once, used by all configs)
# ---------------------------------------------------------------------------
echo ""
echo "--- Building yaddnsc ---"
cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
    -DYADDNSC_USE_NATIVE_DNS=ON \
    -DYADDNSC_MIN_UPDATE_INTERVAL=1 2>&1 | tail -5
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target yaddnsc simple 2>&1 | tail -5

# ca.pem in project root — relative lookup from process cwd
cp "${CERT_FILE}" "${PROJECT_DIR}/ca.pem"
echo "Certificate installed: ${PROJECT_DIR}/ca.pem"

DRIVER_DIR="${BUILD_DIR}/driver/simple"

# ---------------------------------------------------------------------------
# 5. Run test scenarios
# ---------------------------------------------------------------------------
run_scenario() {
    local name="$1"    # scenario name
    local config="$2"  # config template path
    local expect_ip="$3"  # expected IP in update
    local expect_tag="$4" # expected tag in URL

    echo ""
    echo "=== Scenario: ${name} ==="

    # Substitute placeholder with actual driver dir
    local tmpfile
    tmpfile=$(mktemp /tmp/yaddnsc-test-XXXXXX.json)
    sed "s|__DRIVER_DIR__|${DRIVER_DIR}|g" "${config}" > "${tmpfile}"

    # Reset sim logs
    curl -sf "http://127.0.0.1:${SIM_API_PORT:-8080}/reset" > /dev/null 2>&1 || true

    # Run yaddnsc
    "${BUILD_DIR}/yaddnsc" run -c "${tmpfile}" -d &
    local pid=$!
    sleep 10
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    echo "yaddnsc exited"

    # Fetch logs
    local logs
    logs=$(curl -sf "http://127.0.0.1:${SIM_API_PORT}/logs")
    echo "Logs: ${logs}"

    # Verify
    if echo "${logs}" | python3 -c "
import sys, json
logs = json.load(sys.stdin)
for req in logs:
    if '${expect_tag}' in req.get('path', '') and 'ip=${expect_ip}' in req.get('path', ''):
        sys.exit(0)
print('FAIL: no match for tag=${expect_tag} ip=${expect_ip}', file=sys.stderr)
sys.exit(1)
"; then
        echo "  ✓ ${name} PASS"
    else
        echo "  ✗ ${name} FAIL"
        PASS=false
    fi
}

# === INTERFACE source + Classic resolver ===
# lo returns 127.0.0.1
run_scenario \
    "iface+classic" \
    "${SCRIPT_DIR}/configs/config.classic.json" \
    "127.0.0.1" \
    "source=iface&resolver=classic"

# === HTTP source + DoT resolver ===
# HTTP IP source fetches 198.51.100.1 from /myip
run_scenario \
    "http+dot" \
    "${SCRIPT_DIR}/configs/config.dot.json" \
    "198.51.100.1" \
    "source=http&resolver=dot"

# === mDNS source + DoH resolver ===
# mDNS resolves test.local → 198.51.100.4
run_scenario \
    "mdns+doh" \
    "${SCRIPT_DIR}/configs/config.doh.json" \
    "198.51.100.4" \
    "source=mdns&resolver=doh"

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
