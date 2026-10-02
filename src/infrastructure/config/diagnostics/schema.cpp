#include "schema.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <glaze/json/schema.hpp>

#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — glz::meta for Config::AppConfig

namespace Config::Diagnostic {
namespace {
[[nodiscard]] JsonKind schema_kind(std::string_view type) noexcept {
    if (type == "string") {
        return JsonKind::STRING;
    }
    if (type == "object") {
        return JsonKind::OBJECT;
    }
    if (type == "array") {
        return JsonKind::ARRAY;
    }
    if (type == "boolean") {
        return JsonKind::BOOLEAN;
    }
    if (type == "number") {
        return JsonKind::NUMBER;
    }
    if (type == "integer") {
        return JsonKind::INTEGER;
    }
    if (type == "null") {
        return JsonKind::NULL_VALUE;
    }
    return JsonKind::NONE;
}

/// The Config JSON schema, built once and reused by every diagnosis.
struct Schema {
    glz::generic root{};
};

[[nodiscard]] const Schema& app_schema() {
    static const Schema schema = [] {
        Schema built;
        std::string text;
        if (const auto ec = glz::write_json_schema<Config::AppConfig>(text)) {
            return built;
        }
        if (const auto ec = glz::read_json(built.root, text)) {
            return Schema{};
        }
        return built;
    }();
    return schema;
}

/// Member @p name of a JSON object, or nullptr when absent.
[[nodiscard]] const glz::generic* member(const glz::generic& node, std::string_view name) {
    if (!node.is_object()) {
        return nullptr;
    }
    const auto& object = node.get_object();
    const auto entry = object.find(std::string(name));
    return entry == object.end() ? nullptr : &entry->second;
}

/// Resolve a "#/$defs/…" reference against the schema document.
[[nodiscard]] const glz::generic* deref(const glz::generic& root, const glz::generic& node) {
    if (!node.is_object()) {
        return &node;
    }
    const auto* ref = member(node, "$ref");
    if (ref == nullptr || !ref->is_string()) {
        return &node;
    }
    const std::string_view target{ref->get_string()};
    constexpr std::string_view PREFIX = "#/$defs/";
    if (!target.starts_with(PREFIX)) {
        return &node;
    }
    const auto* defs = member(root, "$defs");
    if (defs == nullptr || !defs->is_object()) {
        return &node;
    }
    const auto* entry = member(*defs, target.substr(PREFIX.size()));
    return entry != nullptr ? entry : &node;
}

/// Collect the "type" fields of @p node and of its anyOf/oneOf branches.
void collect_types(const glz::generic& root, const glz::generic& node, std::vector<std::string>& out) {
    const auto* resolved = deref(root, node);
    if (!resolved->is_object()) {
        return;
    }
    if (const auto* type = member(*resolved, "type"); type != nullptr) {
        if (type->is_string()) {
            out.emplace_back(type->get_string());
        } else if (type->is_array()) {
            for (const auto& entry : type->get_array()) {
                if (entry.is_string()) {
                    out.emplace_back(entry.get_string());
                }
            }
        }
    }
    for (const auto* branch_name : {"anyOf", "oneOf"}) {
        const auto* branches = member(*resolved, branch_name);
        if (branches == nullptr || !branches->is_array()) {
            continue;
        }
        for (const auto& branch : branches->get_array()) {
            collect_types(root, branch, out);
        }
    }
}

/// Collect the "const" values of @p node and of its anyOf/oneOf branches.
void collect_values(const glz::generic& root, const glz::generic& node, std::vector<std::string>& out) {
    const auto* resolved = deref(root, node);
    if (!resolved->is_object()) {
        return;
    }
    if (const auto* constant = member(*resolved, "const"); constant != nullptr && constant->is_string()) {
        out.emplace_back(constant->get_string());
    }
    for (const auto* branch_name : {"anyOf", "oneOf"}) {
        const auto* branches = member(*resolved, branch_name);
        if (branches == nullptr || !branches->is_array()) {
            continue;
        }
        for (const auto& branch : branches->get_array()) {
            collect_values(root, branch, out);
        }
    }
}

/// Summarise what @p node accepts.
[[nodiscard]] Expectation expectation_of(const glz::generic& root, const glz::generic& node) {
    Expectation out;
    std::vector<std::string> types;
    collect_types(root, node, types);

    std::vector<std::string> concrete;
    for (auto& type : types) {
        if (type == "null") {
            out.nullable = true;
        } else if (std::ranges::find(concrete, type) == concrete.end()) {
            concrete.push_back(type);
        }
    }
    // A union of kinds is not a single expectation, so it says nothing.
    if (concrete.size() == 1) {
        out.type = schema_kind(concrete.front());
    }

    collect_values(root, node, out.values);
    // Keep the declared order; drop the repeats an aliasing enum produces.
    std::vector<std::string> unique;
    for (auto& value : out.values) {
        if (std::ranges::find(unique, value) == unique.end()) {
            unique.push_back(value);
        }
    }
    out.values = std::move(unique);
    return out;
}

/// Property schema named @p name below @p node.
[[nodiscard]] const glz::generic* property_of(const glz::generic& root, const glz::generic& node,
                                              std::string_view name) {
    const auto* resolved = deref(root, node);
    const auto* properties = member(*resolved, "properties");
    return properties != nullptr ? member(*properties, name) : nullptr;
}

/// Follow exact key/index components, including keys containing dots or brackets.
[[nodiscard]] const glz::generic* resolve_path(const Schema& schema, const Path& path) {
    const glz::generic* node = &schema.root;
    for (const auto& component : path) {
        if (node == nullptr) {
            break;
        }
        node = component.is_index ? member(*deref(schema.root, *node), "items")
                                  : property_of(schema.root, *node, component.key);
    }
    return node;
}

/// Member names accepted by the object @p path points at.
[[nodiscard]] std::vector<std::string> member_names(const Schema& schema, const Path& path) {
    std::vector<std::string> names;
    const auto* node = resolve_path(schema, path);
    if (node == nullptr) {
        return names;
    }
    const auto* properties = member(*deref(schema.root, *node), "properties");
    if (properties == nullptr || !properties->is_object()) {
        return names;
    }
    for (const auto& [name, schema_node] : properties->get_object()) {
        static_cast<void>(schema_node);
        names.push_back(name);
    }
    return names;
}

}  // namespace

Expectation expectation_for(const Path& path) {
    const auto& schema = app_schema();
    const auto* node = resolve_path(schema, path);
    return node == nullptr ? Expectation{} : expectation_of(schema.root, *node);
}

std::vector<std::string> member_names_for(const Path& object_path) {
    return member_names(app_schema(), object_path);
}

}  // namespace Config::Diagnostic
