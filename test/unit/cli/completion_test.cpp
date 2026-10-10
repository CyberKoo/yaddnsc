//
// Shell completion scripts — the command/alias set stays in sync with the
// parser. Pure file-content check against template/, no production code.
//
// Each script is checked for format-specific patterns (a bare substring like
// "r" would match anything): zsh alias cases "resolve|r)", the bash word
// walk list, fish's "__fish_seen_subcommand_from ... r" pairs.
// =============================================================================

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

TEST(CliCompletionTest, ScriptsCoverEveryCommandAndAlias) {
    const std::filesystem::path template_dir = YADDNSC_TEMPLATE_DIR;

    const std::vector<std::pair<std::string, std::vector<std::string>>> expectations = {
        {"zsh/_yaddnsc",
         {"'run:Run the DDNS client'", "'driver:Manage DDNS driver modules'", "'interface:Query network interfaces'",
          "'dns:DNS lookup and diagnostics'", "'config:Configuration management'", "'info:Show build configuration'",
          "interface|if|net)", "resolve|r)", "show|s)", "test|t)", "--type", "--debug", "--quiet", "--config",
          "--version"}},
        {"bash/yaddnsc",
         {"run driver interface dns config info", "interface|if|net)", "resolve|resolver|show|test|r|s|t", "--config",
          "--debug", "--quiet", "--type", "-v"}},
        {"fish/yaddnsc.fish",
         {"__fish_seen_subcommand_from run driver interface if net dns config info", "resolve r", "show s", "test t",
          "-l config", "-l debug", "-l quiet", "-l type", "-l version"}},
    };

    for (const auto& [script, patterns] : expectations) {
        std::ifstream in(template_dir / script);
        ASSERT_TRUE(in.good()) << "missing completion script: " << script;
        const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        for (const auto& pattern : patterns) {
            EXPECT_TRUE(content.find(pattern) != std::string::npos)
                << script << " is out of sync with the parser (missing: " << pattern << ")";
        }
    }
}
