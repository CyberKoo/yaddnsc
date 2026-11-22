#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

runtime=(/packages/yaddnsc_[0-9]*.deb)
devel=(/packages/yaddnsc-dev_*.deb)
upgrade_v1=(/deb-upgrade/v1/yaddnsc_[0-9]*.deb)
upgrade_v2=(/deb-upgrade/v2/yaddnsc_[0-9]*.deb)

for package in "${runtime[@]}" "${devel[@]}" "${upgrade_v1[@]}" "${upgrade_v2[@]}"; do
    if [[ ! -f "${package}" ]]; then
        echo "Missing expected DEB: ${package}" >&2
        exit 1
    fi
done
if [[ ${#runtime[@]} -ne 1 || ${#devel[@]} -ne 1 || \
      ${#upgrade_v1[@]} -ne 1 || ${#upgrade_v2[@]} -ne 1 ]]; then
    echo "Expected exactly one DEB of each type" >&2
    exit 1
fi

if [[ -n "${EXPECTED_PACKAGE_VERSION:-}" ]]; then
    [[ "$(dpkg-deb -f "${runtime[0]}" Version)" == "${EXPECTED_PACKAGE_VERSION}" ]]
fi
dpkg --compare-versions '1.2.0~dev.140.gaaaaaaa' lt '1.2.0~dev.141.gbbbbbbb'
dpkg --compare-versions '1.2.0~dev.141.gbbbbbbb' lt '1.2.0'
dpkg --compare-versions '1.2.0' lt '1.3.0~dev.142.gccccccc'
dpkg --compare-versions '1.3.0~dev.142.gccccccc' lt '1.3.0'

dpkg-deb --contents "${runtime[0]}" | grep -F '/lib/systemd/system/yaddnsc.service'

apt-get update
apt-get install -y --no-install-recommends build-essential cmake /packages/*.deb
. /etc/os-release
if [[ "${ID}" == "ubuntu" && "${VERSION_ID}" == "24.04" ]]; then
    apt-get install -y --no-install-recommends gcc-14 g++-14
    export CC=gcc-14 CXX=g++-14
fi

dpkg-query -W yaddnsc yaddnsc-dev
conffiles="$(dpkg-query -W -f='${Conffiles}' yaddnsc)"
[[ "${conffiles}" == *"/etc/yaddnsc/config.json"* ]]
yaddnsc --version
for driver in \
    alibaba_cloud cloudflare digital_ocean dnspod duckdns godaddy \
    linode namecheap porkbun route53 simple vultr; do
    yaddnsc driver info "${driver}" -c /etc/yaddnsc/config.json >/dev/null
done
yaddnsc config test -c /etc/yaddnsc/config.json -q
cmake -DSDK_PREFIX=/usr -DWORK_DIR=/tmp/sdk-check -DVERIFY_OLD_CXX_ABI=ON \
    -P /usr/share/yaddnsc/examples/verify.cmake

apt-get remove -y yaddnsc-dev yaddnsc
apt-get install -y --no-install-recommends "${upgrade_v1[0]}"
[[ "$(dpkg-query -W -f='${Version}' yaddnsc)" == *"~upgrade.1" ]]

original_config="$(< /etc/yaddnsc/config.json)"
modified_config="${original_config/your-token-here/deb-upgrade-preservation-check}"
[[ "${modified_config}" != "${original_config}" ]]
printf '%s\n' "${modified_config}" > /etc/yaddnsc/config.json

apt-get install -y --no-install-recommends "${upgrade_v2[0]}"
[[ "$(dpkg-query -W -f='${Version}' yaddnsc)" == *"~upgrade.2" ]]
upgraded_config="$(< /etc/yaddnsc/config.json)"
[[ "${upgraded_config}" == "${modified_config}" ]]
yaddnsc config test -c /etc/yaddnsc/config.json -q
echo "DEB upgrade preserved the modified config.json"
