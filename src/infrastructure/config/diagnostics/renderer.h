#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_RENDERER_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_RENDERER_H

#include <string>
#include <string_view>

namespace Config {
namespace Diagnostic {
struct Diagnosis;
}  // namespace Diagnostic
}  // namespace Config

namespace Config::Diagnostic {

/// Render an already-decided diagnosis. Does not inspect input, Glaze, or schema.
[[nodiscard]] std::string render(std::string_view config_path, const Diagnosis& diagnosis);

}  // namespace Config::Diagnostic

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_DIAGNOSTICS_RENDERER_H
