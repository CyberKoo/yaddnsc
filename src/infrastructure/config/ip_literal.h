//
// config — one rule for "this config field holds an IP literal".
//

#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_IP_LITERAL_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_IP_LITERAL_H

#include <optional>
#include <string_view>

class Uri;

namespace Config {

/// The bare address of a config field that holds an IP literal.
///
/// Accepts the bare form ("1.1.1.1", "2606:4700:4700::1111") and the
/// RFC 3986 IP-literal form ("[2606:4700:4700::1111]"), whose brackets are
/// URI syntax rather than part of the address. The returned view points into
/// `uri`'s own buffer, so it stays valid while `uri` lives.
///
/// Returns std::nullopt when the authority host is not an IP literal. Scheme
/// and port are deliberately *not* judged here: a caller that carries them
/// (a DoH / DoT endpoint) inspects them itself, and a caller that has no
/// field to carry them rejects them explicitly rather than through this
/// predicate.
///
/// Single source of truth so the validator, the normalizer and the runtime
/// cannot disagree about which spellings are acceptable — every entry path
/// (run, config test, dns resolve) accepts and rejects exactly the same set.
[[nodiscard]] std::optional<std::string_view> bare_ip_host(const Uri& uri);

}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_IP_LITERAL_H
