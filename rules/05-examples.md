# Examples (Non-normative)

The numbered implementation and error-handling rules are authoritative. This document illustrates checked results, ownership/RAII, and C-string adaptation; it does not introduce additional requirements. Architecture-specific examples are documented at their actual interfaces rather than through placeholder paths or logging macros:

- [Layers and shared utility ownership](../docs/architecture.md#layers).
- [Layer-specific logging entry points](04-quality-and-process.md#layered-logging-policy).
- [Plugin ABI and SDK usage](../docs/custom-drivers.md).
- [Contracts, expected, and exception boundaries](03-error-handling.md).

## Checked Results & RAII

This complete C++23 example uses only the standard library. In project code, reuse the existing result/resource adapters rather than introduce a second file abstraction.

```cpp
#include <cerrno>
#include <cstdio>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace examples {

enum class MathError { DIVISION_BY_ZERO, OVERFLOW };

[[nodiscard]] std::expected<int, MathError> divide(int numerator, int denominator) noexcept {
    if (denominator == 0) {
        return std::unexpected(MathError::DIVISION_BY_ZERO);
    }
    if (numerator == std::numeric_limits<int>::min() && denominator == -1) {
        return std::unexpected(MathError::OVERFLOW);
    }
    return numerator / denominator;
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file != nullptr) {
            // Best-effort release for this read-only example; never throw.
            std::fclose(file);
        }
    }
};

using File = std::unique_ptr<std::FILE, FileCloser>;

struct FileError {
    enum class Code { INVALID_PATH, OPEN_FAILED };
    Code code;
    int system_error;
};

[[nodiscard]] std::expected<File, FileError> open_read_only(std::string_view path) {
    if (path.find('\0') != std::string_view::npos) {
        return std::unexpected(FileError{FileError::Code::INVALID_PATH, 0});
    }
    // The view need not include a terminator. Copy without probing past its end.
    const std::string owned_path(path);
    File file(std::fopen(owned_path.c_str(), "r"));
    if (!file) {
        const int saved_errno = errno;
        return std::unexpected(FileError{FileError::Code::OPEN_FAILED, saved_errno});
    }
    return file;
}

}  // namespace examples
```

- `divide` checks both undefined integer-division cases and can report errors without throwing.
- `open_read_only` may throw while constructing the owned string, so it is not `noexcept`. Operational open failures are error values; `errno` remains confined to the C adapter. Its detailed meaning depends on the platform's C library contract.
- The returned file owner closes on all exit paths, including later exceptions. A write/flush API would need an explicit way to report errors that cannot be conveyed by a destructor.
- A moved-from owner is used only according to its type contract; do not assume that arbitrary moved-from objects are empty. Explicit moves from members or dereferenced owners can be appropriate, while `return std::move(local)` prevents NRVO for an eligible named local.

## Boundary Failure Policies

For concrete boundary implementations, see `include/yaddnsc/sdk/driver.hpp` and the DNS resolver adapters under `src/infrastructure/dns/resolver/`. These illustrate two distinct contracts:

- The plugin C ABI contains C++ exceptions and reports ABI status/error data without allowing an exception to escape.
- DNS resolver APIs translate permitted internal parser exceptions into caller-handled results. Malformed network input is not an internal assertion failure or a process-termination requirement.

Rollback examples must establish that restoration itself cannot throw; copying snapshots alone does not prove strong exception safety. Prefer a real transaction/commit design over an illustrative assignment-based rollback with unstated assumptions.
