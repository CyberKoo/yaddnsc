#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 <deb|rpm|apk> <matrix-id> <input-dir> <output-dir>" >&2
    exit 2
fi

BUILDER=$1
MATRIX_ID=$2
INPUT_DIR=$3
OUTPUT_DIR=$4

mkdir -p "${OUTPUT_DIR}"
shopt -s nullglob
packages=("${INPUT_DIR}"/*."${BUILDER}")
if [[ ${#packages[@]} -eq 0 ]]; then
    echo "No .${BUILDER} packages found in ${INPUT_DIR}" >&2
    exit 1
fi

for package in "${packages[@]}"; do
    filename="${package##*/}"
    case "${BUILDER}:${filename}" in
        deb:yaddnsc-dev_*.deb)
            asset="yaddnsc-dev_${filename##*_}"
            ;;
        deb:yaddnsc_*.deb)
            asset="yaddnsc_${filename##*_}"
            ;;
        rpm:yaddnsc-devel-*.rpm)
            arch="${filename%.rpm}"
            asset="yaddnsc-devel-${arch##*.}.rpm"
            ;;
        rpm:yaddnsc-*.rpm)
            arch="${filename%.rpm}"
            asset="yaddnsc-${arch##*.}.rpm"
            ;;
        apk:yaddnsc-dev-*.apk)
            asset="yaddnsc-dev.apk"
            ;;
        apk:yaddnsc-openrc-*.apk)
            asset="yaddnsc-openrc.apk"
            ;;
        apk:yaddnsc-*.apk)
            asset="yaddnsc.apk"
            ;;
        *)
            echo "Unrecognized ${BUILDER} package: ${filename}" >&2
            exit 1
            ;;
    esac

    destination="${OUTPUT_DIR}/${MATRIX_ID}-${asset}"
    if [[ -e "${destination}" ]]; then
        echo "Duplicate release asset: ${destination}" >&2
        exit 1
    fi
    mv "${package}" "${destination}"
done
