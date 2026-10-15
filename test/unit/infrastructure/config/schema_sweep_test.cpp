//
// Unit tests for the config diagnostics schema lookup
// (src/infrastructure/config/diagnostics/schema.cpp).
//
// expectation_for() and member_names_for() walk a generated schema of the
// real AppConfig. The path-walking arms are per type and per step, so they are
// covered here by walking the schema itself rather than by hand-picking paths:
// member_names_for() enumerates an object's members and expectation_for() names
// the type at each node, so a breadth-first sweep visits every member of every
// object, every element of every array, and every leaf type in one pass.
//
// The sweep is also the strongest available statement that the diagnostics
// layer agrees with the configuration it describes: if a member exists in the
// schema, looking it up must succeed, and an unknown member must not.
//
// Pure lookup over the compiled-in schema — no file, no JSON parsing, no I/O.
//

#include <cstddef>
#include <algorithm>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "infrastructure/config/diagnostics/schema.h"
#include "infrastructure/config/diagnostics/types.h"

namespace {

using namespace Config::Diagnostic;

/// The child component of a path step, given what the schema says the node is.
[[nodiscard]] std::vector<PathComponent> children_of(const Path& path, JsonKind kind) {
    if (kind == JsonKind::OBJECT) {
        std::vector<PathComponent> children;
        for (const auto& name : member_names_for(path)) {
            children.push_back(PathComponent{.key = std::string(name)});
        }
        return children;
    }
    if (kind == JsonKind::ARRAY) {
        // Arrays are indexed; element 0 is enough to ask about the element type.
        return {PathComponent{.key = {}, .index = 0, .is_index = true}};
    }
    return {};
}

/// Walk every node of the schema, calling @p visit at each.
template<typename Visit>
void walk_schema(Visit&& visit) {
    std::deque<Path> pending{Path{}};
    std::size_t visited = 0;
    while (!pending.empty()) {
        const Path path = std::move(pending.front());
        pending.pop_front();
        ++visited;

        const Expectation expectation = expectation_for(path);
        visit(path, expectation);

        for (const auto& child : children_of(path, expectation.type)) {
            Path next = path;
            next.push_back(child);
            pending.push_back(std::move(next));
        }
    }
    // A schema sweep that visited almost nothing would pass every assertion
    // below vacuously, so the traversal itself is asserted to have teeth.
    EXPECT_GT(visited, 20U) << "schema sweep visited too few nodes to mean anything";
}

[[nodiscard]] std::string render(const Path& path) {
    std::string out;
    for (const auto& component : path) {
        if (!out.empty()) {
            out += '.';
        }
        out += component.is_index ? "[]" : component.key;
    }
    return out.empty() ? "<root>" : out;
}

}  // namespace

TEST(DiagnosticSchemaSweep, EveryMemberOfEveryObjectResolvesToAType) {
    std::size_t objects = 0;
    std::size_t arrays = 0;
    std::size_t leaves = 0;

    // Members whose schema carries no JSON type name. driver_params is a
    // glz::generic — an opaque "any JSON" payload — so the generated schema
    // gives it no type and the lookup correctly answers NONE. Anything else
    // answering NONE would mean the walk silently gave up, so this set is
    // pinned: a new untyped member fails here.
    std::vector<std::string> untyped;
    walk_schema([&](const Path& path, const Expectation& expectation) {
        if (expectation.type == JsonKind::NONE) {
            if (!path.empty()) {
                untyped.push_back(render(path));
            }
            return;
        }
        switch (expectation.type) {
            case JsonKind::OBJECT:
                ++objects;
                // An object must name at least one member, or the sweep stops
                // here and the subtree is never reached.
                EXPECT_FALSE(member_names_for(path).empty()) << "object at " << render(path) << " has no members";
                break;
            case JsonKind::ARRAY:
                ++arrays;
                break;
            default:
                ++leaves;
                break;
        }
    });

    EXPECT_GT(objects, 3U);
    EXPECT_GT(arrays, 0U);
    EXPECT_GT(leaves, 5U);

    std::sort(untyped.begin(), untyped.end());
    EXPECT_THAT(untyped, testing::ElementsAre("domains.[].subdomains.[].driver_params"))
        << "a member outside the known opaque set stopped resolving to a type";
}

TEST(DiagnosticSchemaSweep, TheWalkReachesTheKnownConfigurationMembers) {
    // Anchor points from the real config shape. If a rename or a schema
    // regeneration moved them, the diagnostics would be pointing at paths no
    // longer written by any config file.
    std::vector<std::string> reached;
    walk_schema([&](const Path& path, const Expectation&) { reached.push_back(render(path)); });

    const auto has = [&reached](std::string_view needle) {
        return std::any_of(reached.begin(), reached.end(),
                           [needle](const std::string& p) { return p.find(needle) != std::string::npos; });
    };

    EXPECT_TRUE(has("drivers"));
    EXPECT_TRUE(has("drivers.driver_dir"));
    EXPECT_TRUE(has("resolver"));
    EXPECT_TRUE(has("domains"));
    EXPECT_TRUE(has("bootstrap_dns"));
}

TEST(DiagnosticSchemaSweep, ElementPathsAddressTheArrayItemType) {
    const Path server_element{{.key = "resolver"}, {.key = "servers"}, {.key = {}, .index = 0, .is_index = true}};
    const Expectation expectation = expectation_for(server_element);

    // Whatever the schema says the element is, it must be a real type and not
    // the NONE that a mismatched path would produce.
    EXPECT_NE(expectation.type, JsonKind::NONE);

    // A high index is still the same element type: the lookup must not depend
    // on the index value.
    const Path far_element{{.key = "resolver"}, {.key = "servers"}, {.key = {}, .index = 99, .is_index = true}};
    EXPECT_EQ(expectation_for(far_element).type, expectation.type);
}

TEST(DiagnosticSchemaSweep, UnknownMembersAndUnknownContainersStayUntyped) {
    const Path unknown_member{{.key = "no_such_top_level"}};
    EXPECT_EQ(expectation_for(unknown_member).type, JsonKind::NONE);
    EXPECT_TRUE(member_names_for(unknown_member).empty());

    const Path unknown_nested{{.key = "drivers"}, {.key = "no_such_member"}};
    EXPECT_EQ(expectation_for(unknown_nested).type, JsonKind::NONE);
    EXPECT_TRUE(member_names_for(unknown_nested).empty());
}

TEST(DiagnosticSchemaSweep, LeafLookupsNeverEnumerateMembers) {
    // A string has no members; asking anyway must not throw or invent any.
    const Path leaf{{.key = "bootstrap_dns"}};
    ASSERT_EQ(expectation_for(leaf).type, JsonKind::STRING);
    EXPECT_TRUE(member_names_for(leaf).empty());
}
