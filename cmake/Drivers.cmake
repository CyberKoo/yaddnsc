# ==============================================================================
# Bundled driver list — the single source of truth.
#
# The set of bundled drivers is an explicit, auditable list — no directory
# globbing. Runtime auto-discovery (skipping foreign libraries in the driver
# directory) is unaffected by this build-time list. Included by driver/ and
# test/driver/ so both sides build the same set.
# ==============================================================================
set(YADDNSC_DRIVERS
    alibaba_cloud
    cloudflare
    digital_ocean
    dnspod
    duckdns
    godaddy
    linode
    namecheap
    porkbun
    route53
    simple
    vultr
)
