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

export SSL_CERT_FILE="${CERT_FILE}"
echo "  Certificate: ${CERT_FILE}"

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

# Determine the loopback interface name.
# Linux uses "lo", macOS/BSD uses "lo0".
case "$(uname -s)" in
    Darwin|*BSD) LOOPBACK_IFACE="lo0" ;;
    *)           LOOPBACK_IFACE="lo" ;;
esac

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

    local tmpfile tmpfile_base yaddnsc_out yaddnsc_out_base
    tmpfile_base=$(mktemp /tmp/yaddnsc-test-XXXXXX)
    tmpfile="${tmpfile_base}.json"
    mv "${tmpfile_base}" "${tmpfile}"
    yaddnsc_out_base=$(mktemp /tmp/yaddnsc-output-XXXXXX)
    yaddnsc_out="${yaddnsc_out_base}.txt"
    mv "${yaddnsc_out_base}" "${yaddnsc_out}"
    sed -e "s|__DRIVER_DIR__|${DRIVER_DIR}|g" \
        -e "s|\"interface\": \"lo\"|\"interface\": \"${LOOPBACK_IFACE}\"|g" "${config}" > "${tmpfile}"

    # Reset sim logs
    curl -sf "http://127.0.0.1:${SIM_API_PORT:-8080}/reset" > /dev/null 2>&1 || true

    # Run yaddnsc with a 4-second total timeout.
    # 2s for the update cycle, then SIGTERM + 2s grace before SIGKILL.
    "${YADDNSC_BIN}" run -c "${tmpfile}" -d > "${yaddnsc_out}" 2>&1 &
    local pid=$!
    sleep 2
    kill "${pid}" 2>/dev/null || true
    sleep 2
    if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
    fi
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
# 6. CLI surface — user-visible contracts
#
# The scenarios above only exercise `yaddnsc run`. These assert the contracts
# that scripts and operators depend on: exit status and machine-readable
# output for the read-only subcommands. They need no update cycle, so they run
# in seconds.
# ---------------------------------------------------------------------------
echo ""
echo "=== CLI surface ==="

# Bound CLI checks even when a regression leaves a command running forever.
# Python is already required here; unlike timeout(1), this also works on macOS.
run_bounded() {
    python3 - "$@" <<'PY'
import subprocess, sys
try:
    result = subprocess.run(sys.argv[1:], timeout=15)
except subprocess.TimeoutExpired:
    print('CLI command timed out', file=sys.stderr)
    sys.exit(124)
sys.exit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
PY
}

# cli_check <name> <zero|nonzero> <output-regex|""> <cmd...>
cli_check() {
    local name="$1"
    local expect="$2"
    local pattern="$3"
    shift 3

    local out rc
    set +e
    out=$(run_bounded "$@" 2>&1)
    rc=$?
    set -e

    local ok=true
    if [ "${rc}" -ge 124 ]; then
        ok=false
        echo "  ✗ ${name}: timed out or terminated abnormally (${rc})"
    fi
    if [ "${expect}" = "zero" ] && [ "${rc}" -ne 0 ]; then
        ok=false
        echo "  ✗ ${name}: expected exit 0, got ${rc}"
        echo "${out}" | tail -5 | sed 's/^/    | /'
    elif [ "${expect}" = "nonzero" ] && [ "${rc}" -eq 0 ]; then
        ok=false
        echo "  ✗ ${name}: expected a non-zero exit, got 0"
    fi
    if [ -n "${pattern}" ] && ! echo "${out}" | grep -qE "${pattern}"; then
        ok=false
        echo "  ✗ ${name}: output does not match /${pattern}/"
        echo "${out}" | tail -5 | sed 's/^/    | /'
    fi

    if [ "${ok}" = true ]; then
        echo "  ✓ ${name} PASS"
    else
        echo "  ✗ ${name} FAIL"
        PASS=false
    fi
}

# A config with the driver path resolved, for the read-only subcommands.
CFG_TMP="${BUILD_DIR}/integration-cli-config.json"
sed -e "s|__DRIVER_DIR__|${DRIVER_DIR}|g" \
    -e "s|\"interface\": \"lo\"|\"interface\": \"${LOOPBACK_IFACE}\"|g" \
    "${SCRIPT_DIR}/configs/config.classic.json" > "${CFG_TMP}"

cli_check "version"        zero    '^yaddnsc/'          "${YADDNSC_BIN}" --version
cli_check "info"           zero    'Build configuration' "${YADDNSC_BIN}" info
cli_check "config-show"    zero    '"driver_dir"'        "${YADDNSC_BIN}" config show -c "${CFG_TMP}"
cli_check "config-test"    zero    'test passed'         "${YADDNSC_BIN}" config test -c "${CFG_TMP}"
cli_check "interface-list" zero    "${LOOPBACK_IFACE}"   "${YADDNSC_BIN}" interface list
cli_check "driver-list"    zero    'simple'              "${YADDNSC_BIN}" driver list -c "${CFG_TMP}"
cli_check "dns-resolver"   zero    'Custom server'       "${YADDNSC_BIN}" dns resolver -c "${CFG_TMP}"

# `config show` must emit parseable JSON, not pretty-printed prose: tooling
# parses it with json.load.
if run_bounded "${YADDNSC_BIN}" config show -c "${CFG_TMP}" 2>/dev/null \
        | python3 -c "import sys, json; json.load(sys.stdin)"; then
    echo "  ✓ config-show-is-json PASS"
else
    echo "  ✗ config-show-is-json FAIL"
    PASS=false
fi

# Negative cases: a malformed config and a missing file must both be reported,
# not silently accepted. These are the two ways a deployment goes wrong before
# yaddnsc ever reaches the network.
echo '{ "driver": { "driver_dir": ' > "${BUILD_DIR}/integration-bad-config.json"
cli_check "config-test-malformed" nonzero 'Failed to validate configuration' \
    "${YADDNSC_BIN}" config test -c "${BUILD_DIR}/integration-bad-config.json"
cli_check "config-test-missing-file" nonzero 'does not exist' \
    "${YADDNSC_BIN}" config test -c "${BUILD_DIR}/integration-no-such-config.json"

rm -f "${CFG_TMP}" "${BUILD_DIR}/integration-bad-config.json"

# ---------------------------------------------------------------------------
# 7. Negative run scenarios
#
# The happy-path scenarios only prove that a successful cycle updates. These
# pin down the two failure contracts, which pull in opposite directions:
#
#   * No address  -> publish nothing. Never send a bogus or empty address to
#                    the provider; that is worse than not updating at all.
#   * No DNS answer -> still publish. update_workflow.cpp treats an
#                    unverifiable current record as an empty one, on the
#                    grounds that pushing an unchanged record is harmless
#                    while skipping a changed one is not. A regression that
#                    turns this into a silent skip would leave a moved host
#                    stuck at its old address.
# ---------------------------------------------------------------------------
echo ""
echo "=== Negative run scenarios ==="

# Populated by run_cycle; initialised so `set -u` cannot trip if it exits early.
LAST_UPDATES=0
LAST_LOG=""
LAST_REQUESTS=""
LAST_CYCLE_OK=false

# A UDP port with nothing bound, so every resolver query times out.
DEAD_DNS_PORT=$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1', 0)); print(s.getsockname()[1]); s.close()")

# run_cycle <name> <template-config> <settle-seconds> <iface|NONE> <dns-port|NONE>
#
# Runs one update cycle and records in LAST_UPDATES how many /update requests
# the provider received. The caller decides whether that count is correct.
# Pass NONE for <iface> to keep the real loopback, or for <dns-port> to keep
# the template's resolver port. The sed overrides are built as an array
# because the patterns contain spaces and must not be word-split.
run_cycle() {
    local name="$1"
    local config="$2"
    local settle="$3"
    local iface="$4"
    local dns_port="$5"

    echo ""
    echo "=== Scenario: ${name} ==="

    [ "${iface}" = "NONE" ] && iface="${LOOPBACK_IFACE}"

    local sed_args=(
        -e "s|__DRIVER_DIR__|${DRIVER_DIR}|g"
        -e "s|\"interface\": \"lo\"|\"interface\": \"${iface}\"|g"
    )
    if [ "${dns_port}" != "NONE" ]; then
        sed_args+=(-e "s|15353|${dns_port}|g")
    fi

    local cfg out pid logs updates rc
    LAST_CYCLE_OK=false
    cfg=$(mktemp /tmp/yaddnsc-neg-XXXXXX.json)
    out=$(mktemp /tmp/yaddnsc-neg-XXXXXX.txt)
    sed "${sed_args[@]}" "${config}" > "${cfg}"

    if ! curl -sf "http://127.0.0.1:${SIM_API_PORT}/reset" > /dev/null; then
        echo "  ✗ ${name}: simulator reset failed"
        PASS=false
        LAST_LOG="${out}"
        rm -f "${cfg}"
        return
    fi

    set +e
    "${YADDNSC_BIN}" run -c "${cfg}" -d > "${out}" 2>&1 &
    pid=$!
    sleep "${settle}"
    kill "${pid}" 2>/dev/null || true
    sleep 1
    if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
    fi
    wait "${pid}" 2>/dev/null
    rc=$?
    set -e
    echo "  yaddnsc exited (status ${rc})"
    LAST_LOG="${out}"
    rm -f "${cfg}"

    if [ "${rc}" -ne 0 ]; then
        echo "  ✗ ${name}: yaddnsc did not shut down successfully"
        PASS=false
        return
    fi
    if ! logs=$(curl -sf "http://127.0.0.1:${SIM_API_PORT}/logs"); then
        echo "  ✗ ${name}: simulator logs request failed"
        PASS=false
        return
    fi
    if ! updates=$(echo "${logs}" | python3 -c "
import sys, json
entries = json.load(sys.stdin)
if not isinstance(entries, list) or any(not isinstance(e, dict) for e in entries):
    raise ValueError('expected a list of request objects')
print(sum(1 for e in entries if '/update' in e.get('path', '')))
"); then
        echo "  ✗ ${name}: invalid simulator logs"
        PASS=false
        return
    fi

    LAST_UPDATES="${updates}"
    LAST_REQUESTS="${logs}"
    LAST_CYCLE_OK=true
}

# ip-source-failure-publishes-nothing
#
# Use a valid HTTP source whose response is not an IP address. Environment
# validation succeeds, but step 1 of the update workflow cannot get an address.
NO_ADDRESS_CFG=$(mktemp /tmp/yaddnsc-no-address-XXXXXX.json)
python3 - "${SCRIPT_DIR}/configs/config.classic.json" "${SIM_API_PORT}" > "${NO_ADDRESS_CFG}" <<'PY'
import json, sys
with open(sys.argv[1]) as source:
    config = json.load(source)
subdomain = config['domains'][0]['subdomains'][0]
subdomain.pop('interface')
subdomain['ip_source'] = 'http'
subdomain['ip_source_param'] = 'http://127.0.0.1:' + sys.argv[2] + '/health'
json.dump(config, sys.stdout)
PY
run_cycle \
    "ip-source-failure-publishes-nothing" \
    "${NO_ADDRESS_CFG}" \
    5 \
    "NONE" \
    "NONE"
rm -f "${NO_ADDRESS_CFG}"
if [ "${LAST_CYCLE_OK}" = true ] && [ "${LAST_UPDATES}" -eq 0 ] &&
        grep -qE 'Failed to resolve local IP address.*skipping the update|No valid IP address found.*skipping the update' "${LAST_LOG}"; then
    echo "  ✓ ip-source-failure-publishes-nothing PASS"
else
    echo "  ✗ ip-source-failure-publishes-nothing FAIL (${LAST_UPDATES} update(s); expected a completed no-address workflow)"
    echo "  Requests: ${LAST_REQUESTS}"
    grep -iE "no valid|skip|address" "${LAST_LOG}" 2>/dev/null | head -5 | sed 's/^/    | /'
    PASS=false
fi
rm -f "${LAST_LOG}"

# resolver-failure-still-publishes
#
# Documents update_workflow.cpp's deliberate trade-off: a resolver that
# cannot answer must not stop the update.
run_cycle \
    "resolver-failure-still-publishes" \
    "${SCRIPT_DIR}/configs/config.classic.json" \
    6 \
    "NONE" \
    "${DEAD_DNS_PORT}"
if [ "${LAST_CYCLE_OK}" = true ] && [ "${LAST_UPDATES}" -ge 1 ] &&
        grep -qE 'DNS lookup.*failed:.*proceeding with update' "${LAST_LOG}"; then
    echo "  ✓ resolver-failure-still-publishes PASS (${LAST_UPDATES} update(s), as designed)"
else
    echo "  ✗ resolver-failure-still-publishes FAIL (an unreachable resolver suppressed the update)"
    grep -iE "resolver|query|failed|proceeding" "${LAST_LOG}" 2>/dev/null | head -5 | sed 's/^/    | /'
    PASS=false
fi
rm -f "${LAST_LOG}"

# A run against a malformed config must exit non-zero promptly instead of
# looping on a config it cannot read.
echo ""
echo "=== Scenario: malformed-config-aborts ==="
echo '{ "driver": { "driver_dir": ' > "${BUILD_DIR}/integration-run-bad.json"
set +e
run_bounded "${YADDNSC_BIN}" run -c "${BUILD_DIR}/integration-run-bad.json" -d > /tmp/yaddnsc-run-bad.log 2>&1
rc=$?
set -e
if [ "${rc}" -gt 0 ] && [ "${rc}" -lt 124 ] && grep -qiE 'parse|validate|failed' /tmp/yaddnsc-run-bad.log; then
    echo "  ✓ malformed-config-aborts PASS (status ${rc})"
else
    echo "  ✗ malformed-config-aborts FAIL (status ${rc})"
    tail -5 /tmp/yaddnsc-run-bad.log | sed 's/^/    | /'
    PASS=false
fi
rm -f "${BUILD_DIR}/integration-run-bad.json" /tmp/yaddnsc-run-bad.log

# ---------------------------------------------------------------------------
# 8. Final result
# ---------------------------------------------------------------------------
echo ""
if [ "${PASS}" = true ]; then
    echo "=== ALL SCENARIOS PASSED ==="
    exit 0
else
    echo "=== SOME SCENARIOS FAILED ==="
    exit 1
fi
