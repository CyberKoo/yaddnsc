#include "parse_diagnostic.h"

#include "infrastructure/config/diagnostics/decision.h"
#include "infrastructure/config/diagnostics/error_adapter.h"
#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/renderer.h"
#include "infrastructure/config/diagnostics/schema.h"

namespace Config::Diagnostic {

std::string describe_parse_error(const std::string& config_path, std::string_view buffer, std::size_t error_offset,
                                 glz::error_code code) {
    const auto failure = adapt_error(code);
    const InputFacts input{.empty = buffer.empty(), .at_end = error_offset >= buffer.size()};
    const auto site = needs_location(failure, input) ? locate(buffer, error_offset, scan_policy(failure)) : Site{};
    SchemaFacts schema;
    switch (schema_need(failure, site, input)) {
        case SchemaNeed::NONE:
            break;
        case SchemaNeed::EXPECTATION:
            schema.expected = expectation_for(site.path);
            break;
        case SchemaNeed::MEMBER_NAMES: {
            auto parent = site.path;
            if (!parent.empty()) {
                parent.pop_back();
            }
            schema.member_names = member_names_for(parent);
            break;
        }
    }
    const auto diagnosis = decide(failure, site, input, schema);
    return render(config_path, diagnosis);
}

}  // namespace Config::Diagnostic
