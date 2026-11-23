#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <project-dir>" >&2
    exit 2
fi

PROJECT_DIR=$1
PREPARE="${PROJECT_DIR}/docker/prepare-nightly-assets.sh"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT
shopt -s nullglob

arm64_matrix="$(awk '
    /^  native-packages-arm64:$/ { in_matrix = 1; next }
    in_matrix && /^  [[:alnum:]_-]+:$/ { exit }
    in_matrix { print }
' "${PROJECT_DIR}/.github/workflows/nightly.yml")"
required_arm64_ids=(
    debian-13-arm64
    ubuntu-24.04-arm64
    ubuntu-26.04-arm64
    fedora-43-arm64
    opensuse-leap-16.0-arm64
    opensuse-tumbleweed-arm64
    alpine-3-arm64
)
for matrix_id in "${required_arm64_ids[@]}"; do
    if ! grep -F -- "- id: ${matrix_id}" <<<"${arm64_matrix}" >/dev/null; then
        echo "Missing AArch64 matrix entry: ${matrix_id}" >&2
        exit 1
    fi
done

assert_assets() {
    local directory=$1
    shift
    local actual=("${directory}"/*)
    local name

    if [[ ${#actual[@]} -ne $# ]]; then
        echo "Expected $# assets in ${directory}, found ${#actual[@]}" >&2
        exit 1
    fi
    for name in "$@"; do
        if [[ "${name}" != yaddnsc-* || ! -f "${directory}/${name}" ]]; then
            echo "Missing or incorrectly named asset: ${name}" >&2
            exit 1
        fi
    done
}

mkdir -p "${WORK_DIR}/deb-input"
touch \
    "${WORK_DIR}/deb-input/yaddnsc_1.2.0~dev.1.gabcdef_arm64.deb" \
    "${WORK_DIR}/deb-input/yaddnsc-dev_1.2.0~dev.1.gabcdef_arm64.deb"
"${PREPARE}" deb ubuntu-24.04-arm64 "${WORK_DIR}/deb-input" "${WORK_DIR}/deb-output"
assert_assets "${WORK_DIR}/deb-output" \
    yaddnsc-ubuntu-24.04-arm64_arm64.deb \
    yaddnsc-ubuntu-24.04-arm64-dev_arm64.deb

mkdir -p "${WORK_DIR}/rpm-input"
touch \
    "${WORK_DIR}/rpm-input/yaddnsc-1.2.0-1.aarch64.rpm" \
    "${WORK_DIR}/rpm-input/yaddnsc-devel-1.2.0-1.aarch64.rpm"
"${PREPARE}" rpm fedora-43-arm64 "${WORK_DIR}/rpm-input" "${WORK_DIR}/rpm-output"
assert_assets "${WORK_DIR}/rpm-output" \
    yaddnsc-fedora-43-arm64-aarch64.rpm \
    yaddnsc-fedora-43-arm64-devel-aarch64.rpm

mkdir -p "${WORK_DIR}/apk-input"
touch \
    "${WORK_DIR}/apk-input/yaddnsc-1.2.0_alpha0_git1-r0.apk" \
    "${WORK_DIR}/apk-input/yaddnsc-dev-1.2.0_alpha0_git1-r0.apk" \
    "${WORK_DIR}/apk-input/yaddnsc-openrc-1.2.0_alpha0_git1-r0.apk"
"${PREPARE}" apk alpine-3-arm64 "${WORK_DIR}/apk-input" "${WORK_DIR}/apk-output"
assert_assets "${WORK_DIR}/apk-output" \
    yaddnsc-alpine-3-arm64.apk \
    yaddnsc-alpine-3-arm64-dev.apk \
    yaddnsc-alpine-3-arm64-openrc.apk

mkdir -p "${WORK_DIR}/empty-input"
if "${PREPARE}" deb debian-13-arm64 "${WORK_DIR}/empty-input" \
    "${WORK_DIR}/empty-output" 2>"${WORK_DIR}/empty-error"; then
    echo "Accepted an empty package directory" >&2
    exit 1
fi
grep -F 'No .deb packages found' "${WORK_DIR}/empty-error" >/dev/null

mkdir -p "${WORK_DIR}/unknown-input"
touch "${WORK_DIR}/unknown-input/other_1.2.0_arm64.deb"
if "${PREPARE}" deb debian-13-arm64 "${WORK_DIR}/unknown-input" \
    "${WORK_DIR}/unknown-output" 2>"${WORK_DIR}/unknown-error"; then
    echo "Accepted an unrecognized package" >&2
    exit 1
fi
grep -F 'Unrecognized deb package' "${WORK_DIR}/unknown-error" >/dev/null

mkdir -p "${WORK_DIR}/duplicate-input"
touch \
    "${WORK_DIR}/duplicate-input/yaddnsc_1.2.0_arm64.deb" \
    "${WORK_DIR}/duplicate-input/yaddnsc_1.2.1_arm64.deb"
if "${PREPARE}" deb debian-13-arm64 "${WORK_DIR}/duplicate-input" \
    "${WORK_DIR}/duplicate-output" 2>"${WORK_DIR}/duplicate-error"; then
    echo "Accepted duplicate release assets" >&2
    exit 1
fi
grep -F 'Duplicate release asset' "${WORK_DIR}/duplicate-error" >/dev/null

echo "Nightly package asset checks passed"
