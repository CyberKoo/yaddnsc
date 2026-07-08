//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_VALIDATION_H
#define YADDNSC_UTIL_VALIDATION_H

#include <regex>
#include <string_view>

namespace Utils
{
/// Maximum length of a fully qualified domain name (RFC 1035).
static constexpr int DOMAIN_NAME_MAX_LEN = 253;

/// Check whether a string is a valid fully-qualified domain name.
///
/// Validates against RFC 1035 / RFC 1123 rules:
///   - Each label is 1–63 characters long.
///   - Labels consist of letters (case-insensitive), digits, and hyphens.
///   - A label must not start or end with a hyphen.
///   - At least one dot is required (rejects bare hostnames).
///   - An optional trailing dot is permitted (fully-qualified form).
///   - The final label (TLD) must be at least 2 alphabetic characters,
///     or a valid punycode label (xn--...).
///   - Total encoded length must not exceed 253 characters.
///
/// @param domain  The domain name string to validate.
/// @return        true if the domain name is syntactically valid.
[[nodiscard]] inline bool is_valid_domain(std::string_view domain)
{
  // Label pattern (RFC 1035 §2.3.1 + RFC 1123 §2.1):
  //   [a-z0-9]          — first character must be alphanumeric
  //   ([a-z0-9-]{0,61}[a-z0-9])?  — optional middle (0–61) + ending alphanumeric
  //   Total: 1 + (0–61 middle + 1 ending) = 1–63 characters.
  //   Hyphens are allowed in the middle but NOT at the start or end.
  //
  // Full regex:
  //   ^(label\.)+            — one or more subdomain labels each followed by a dot
  //    (TLD)\.?$             — final label (TLD) with optional trailing dot
  //
  // TLD alternatives:
  //   [a-z]{2,}              — at least 2 alpha characters (e.g. "com", "io", "travel")
  //   xn--...                — punycode-encoded internationalised TLD
  const static std::regex domain_regex(
      R"(^([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+([a-z]{2,}|xn--[a-z0-9]([a-z0-9-]{0,59}[a-z0-9])?)\.?$)",
      std::regex::icase);

  if (domain.length() > DOMAIN_NAME_MAX_LEN) {
    return false;
  }

  if (domain.find('.') != std::string_view::npos) {
    std::match_results<std::string_view::const_iterator> match;
    std::regex_match(domain.begin(), domain.end(), match, domain_regex);

    return !match.empty();
  }

  return false;
}
}  // namespace Utils

#endif  // YADDNSC_UTIL_VALIDATION_H
