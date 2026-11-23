#!/usr/bin/env bash
set -euo pipefail

runtime=(/packages/yaddnsc-[0-9]*.rpm)
devel=(/packages/yaddnsc-devel-*.rpm)
if [[ ${#runtime[@]} -ne 1 || ! -f "${runtime[0]}" ]]; then
    echo "Expected exactly one runtime RPM" >&2
    exit 1
fi
if [[ ${#devel[@]} -ne 1 || ! -f "${devel[0]}" ]]; then
    echo "Expected exactly one development RPM" >&2
    exit 1
fi
if [[ -n "${EXPECTED_PACKAGE_VERSION:-}" ]]; then
    [[ "$(rpm -qp --qf '%{VERSION}' "${runtime[0]}")" == "${EXPECTED_PACKAGE_VERSION}" ]]
fi

upgrade_v1=(/rpm-upgrade/v1/yaddnsc-[0-9]*.rpm)
upgrade_v2=(/rpm-upgrade/v2/yaddnsc-[0-9]*.rpm)
if [[ ${#upgrade_v1[@]} -ne 1 || ! -f "${upgrade_v1[0]}" || \
      ${#upgrade_v2[@]} -ne 1 || ! -f "${upgrade_v2[0]}" ]]; then
    echo "Expected one runtime RPM in each upgrade directory" >&2
    exit 1
fi

config_found=0
dump="$(rpm -qp --dump "${runtime[0]}")"
while read -r path size mtime digest mode owner group is_config remainder; do
    if [[ "${path}" == "/etc/yaddnsc/config.json" ]]; then
        config_found=1
        [[ "${is_config}" == "1" ]]
    fi
done <<< "${dump}"
[[ "${config_found}" == "1" ]]

service_found=0
license_found=0
contents="$(rpm -qpl "${runtime[0]}")"
while read -r path; do
    case "${path}" in
        /usr/lib/systemd/system/yaddnsc.service) service_found=1 ;;
        /usr/share/doc/yaddnsc/LICENSE) license_found=1 ;;
    esac
done <<< "${contents}"
[[ "${service_found}" == "1" && "${license_found}" == "1" ]]

scripts="$(rpm -qp --scripts "${runtime[0]}")"
[[ "${scripts}" == *"daemon-reload"* ]]
[[ "${scripts}" == *"stop yaddnsc.service"* ]]
[[ "${scripts}" == *"disable yaddnsc.service"* ]]

install_packages() {
    if command -v dnf >/dev/null 2>&1; then
        dnf -y --setopt=install_weak_deps=False install "$@"
    elif command -v zypper >/dev/null 2>&1; then
        zypper --non-interactive install --no-recommends --allow-unsigned-rpm "$@"
    else
        echo "Unsupported RPM distribution: dnf or zypper is required" >&2
        exit 1
    fi
}

if command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive refresh
fi
install_packages gcc-c++ cmake make /packages/*.rpm

rpm -q yaddnsc yaddnsc-devel
version="$(yaddnsc --version)"
printf '%s\n' "${version}"
[[ "${version}" =~ ^yaddnsc/[0-9]+\.[0-9]+\.[0-9]+-.+ ]]
for driver in \
    alibaba_cloud cloudflare digital_ocean dnspod duckdns godaddy \
    linode namecheap porkbun route53 simple vultr; do
    yaddnsc driver info "${driver}" -c /etc/yaddnsc/config.json >/dev/null
done
yaddnsc config test -c /etc/yaddnsc/config.json -q
cmake -DSDK_PREFIX=/usr -DWORK_DIR=/tmp/sdk-check -DVERIFY_OLD_CXX_ABI=ON \
    -P /usr/share/yaddnsc/examples/verify.cmake

rpm -e yaddnsc-devel yaddnsc
install_packages "${upgrade_v1[0]}"
[[ "$(rpm -q --qf '%{RELEASE}' yaddnsc)" == 1* ]]

original_config="$(< /etc/yaddnsc/config.json)"
modified_config="${original_config/your-token-here/rpm-upgrade-preservation-check}"
[[ "${modified_config}" != "${original_config}" ]]
printf '%s\n' "${modified_config}" > /etc/yaddnsc/config.json

install_packages "${upgrade_v2[0]}"
[[ "$(rpm -q --qf '%{RELEASE}' yaddnsc)" == 2* ]]
upgraded_config="$(< /etc/yaddnsc/config.json)"
[[ "${upgraded_config}" == "${modified_config}" ]]
yaddnsc config test -c /etc/yaddnsc/config.json -q
echo "RPM upgrade preserved the modified config.json"
