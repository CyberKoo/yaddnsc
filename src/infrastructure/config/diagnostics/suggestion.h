#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_SUGGESTION_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_SUGGESTION_H

#include <string>
#include <vector>

namespace Config::Diagnostic {

/// Closest candidate to @p name, or "" when no candidate is close or two tie.
///
/// Backs the "did you mean" clause of an unknown-key message: the caller pairs
/// the returned spelling with the candidates it already lists.
[[nodiscard]] std::string closest_key(const std::string& name, const std::vector<std::string>& candidates);

}  // namespace Config::Diagnostic

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_SUGGESTION_H
