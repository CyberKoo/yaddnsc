#!/usr/bin/env bash
set -euo pipefail

runtime=(/packages/yaddnsc-[0-9]*.apk)
devel=(/packages/yaddnsc-dev-*.apk)
openrc=(/packages/yaddnsc-openrc-*.apk)
upgrade_v1=(/apk-upgrade/v1/yaddnsc-[0-9]*.apk)
upgrade_v2=(/apk-upgrade/v2/yaddnsc-[0-9]*.apk)

for package in "${runtime[@]}" "${devel[@]}" "${openrc[@]}" \
    "${upgrade_v1[@]}" "${upgrade_v2[@]}"; do
    if [[ ! -f "${package}" ]]; then
        echo "Missing expected APK: ${package}" >&2
        exit 1
    fi
done
if [[ ${#runtime[@]} -ne 1 || ${#devel[@]} -ne 1 || ${#openrc[@]} -ne 1 || \
      ${#upgrade_v1[@]} -ne 1 || ${#upgrade_v2[@]} -ne 1 ]]; then
    echo "Expected exactly one APK of each type" >&2
    exit 1
fi

apk add --no-cache --allow-untrusted \
    build-base cmake /packages/*.apk

id yaddnsc
[[ -x /etc/init.d/yaddnsc ]]
[[ -f /etc/conf.d/yaddnsc ]]
sh -n /etc/init.d/yaddnsc

yaddnsc --version
for driver in \
    alibaba_cloud cloudflare digital_ocean dnspod duckdns godaddy \
    linode namecheap porkbun route53 simple vultr; do
    yaddnsc driver info "${driver}" -c /etc/yaddnsc/config.json >/dev/null
done
yaddnsc config test -c /etc/yaddnsc/config.json -q
cmake -DSDK_PREFIX=/usr -DWORK_DIR=/tmp/sdk-check -DVERIFY_OLD_CXX_ABI=ON \
    -P /usr/share/yaddnsc/examples/verify.cmake

apk del yaddnsc-openrc yaddnsc-dev yaddnsc
apk add --no-cache --allow-untrusted "${upgrade_v1[0]}"
[[ "$(apk list --installed yaddnsc)" == *"-r0 "* ]]

original_config="$(< /etc/yaddnsc/config.json)"
modified_config="${original_config/your-token-here/apk-upgrade-preservation-check}"
[[ "${modified_config}" != "${original_config}" ]]
printf '%s\n' "${modified_config}" > /etc/yaddnsc/config.json

apk add --no-cache --allow-untrusted "${upgrade_v2[0]}"
[[ "$(apk list --installed yaddnsc)" == *"-r1 "* ]]
upgraded_config="$(< /etc/yaddnsc/config.json)"
[[ "${upgraded_config}" == "${modified_config}" ]]
yaddnsc config test -c /etc/yaddnsc/config.json -q
echo "APK upgrade preserved the modified config.json"
