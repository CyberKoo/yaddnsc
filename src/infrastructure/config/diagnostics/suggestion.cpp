#include "suggestion.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Config::Diagnostic {
namespace {
/// Levenshtein distance; small inputs only (configuration key names).
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> previous(b.size() + 1);
    std::vector<std::size_t> current(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U);
            current[j] = std::min({substitution, previous[j] + 1, current[j - 1] + 1});
        }
        previous.swap(current);
    }
    return previous[b.size()];
}
}  // namespace

std::string closest_key(const std::string& name, const std::vector<std::string>& candidates) {
    const std::size_t limit = (std::max) (std::size_t{2}, name.size() / 3);
    std::string best;
    std::size_t best_distance = limit + 1;
    bool tied{false};
    for (const auto& candidate : candidates) {
        const auto distance = edit_distance(name, candidate);
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
            tied = false;
        } else if (distance == best_distance) {
            tied = true;
        }
    }
    return (best_distance <= limit && !tied) ? best : std::string{};
}

}  // namespace Config::Diagnostic
