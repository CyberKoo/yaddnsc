#include "bootstrap.h"

#include <exception>
#include <variant>

#include "cli/presenter.h"
#include "composition/commands/diagnostics.h"
#include "composition/commands/run.h"

namespace {
struct Dispatch {
    // Run owns startup logging; defects reach main's process boundary.
    int operator()(const Cli::RunCommand& command) const { return Composition::execute_command(command); }

    // Diagnostic assembly failures share the plain stderr presentation policy.
    template<typename Command>
    int operator()(const Command& command) const {
        try {
            return Composition::execute_command(command);
        } catch (const std::exception& error) {
            return Cli::present_error(error);
        }
    }
};
}  // namespace

int Composition::dispatch(const Cli::Command& command) {
    return std::visit(Dispatch{}, command);
}
