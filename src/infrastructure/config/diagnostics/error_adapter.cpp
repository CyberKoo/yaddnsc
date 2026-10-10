#include "error_adapter.h"

#include <glaze/glaze.hpp>
#include <string>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace Config::Diagnostic {

ParseFailure adapt_error(glz::error_code code) {
    switch (code) {
        case glz::error_code::expected_brace:
            return {.kind = ErrorKind::EXPECTED_BRACE, .identifier = {}};
        case glz::error_code::expected_bracket:
            return {.kind = ErrorKind::EXPECTED_BRACKET, .identifier = {}};
        case glz::error_code::expected_quote:
            return {.kind = ErrorKind::EXPECTED_QUOTE, .identifier = {}};
        case glz::error_code::expected_comma:
            return {.kind = ErrorKind::EXPECTED_COMMA, .identifier = {}};
        case glz::error_code::expected_colon:
            return {.kind = ErrorKind::EXPECTED_COLON, .identifier = {}};
        case glz::error_code::unexpected_end:
            return {.kind = ErrorKind::UNEXPECTED_END, .identifier = {}};
        case glz::error_code::syntax_error:
            return {.kind = ErrorKind::SYNTAX, .identifier = {}};
        case glz::error_code::parse_number_failure:
            return {.kind = ErrorKind::NUMBER, .identifier = {}};
        case glz::error_code::invalid_escape:
            return {.kind = ErrorKind::INVALID_ESCAPE, .identifier = {}};
        case glz::error_code::u_requires_hex_digits:
            return {.kind = ErrorKind::INCOMPLETE_UNICODE, .identifier = {}};
        case glz::error_code::unicode_escape_conversion_failure:
            return {.kind = ErrorKind::INVALID_UNICODE, .identifier = {}};
        case glz::error_code::exceeded_max_recursive_depth:
            return {.kind = ErrorKind::DEPTH, .identifier = {}};
        case glz::error_code::no_read_input:
            return {.kind = ErrorKind::EMPTY, .identifier = {}};
        case glz::error_code::file_open_failure:
            return {.kind = ErrorKind::FILE_OPEN, .identifier = {}};
        case glz::error_code::file_close_failure:
            return {.kind = ErrorKind::FILE_CLOSE, .identifier = {}};
        case glz::error_code::file_include_error:
            return {.kind = ErrorKind::FILE_INCLUDE, .identifier = {}};
        case glz::error_code::file_extension_not_supported:
            return {.kind = ErrorKind::FILE_EXTENSION, .identifier = {}};
        case glz::error_code::missing_key:
            return {.kind = ErrorKind::MISSING_KEY, .identifier = {}};
        case glz::error_code::key_not_found:
            return {.kind = ErrorKind::MISSING_KEY, .identifier = {}};
        case glz::error_code::invalid_nullable_read:
            return {.kind = ErrorKind::INVALID_NULL, .identifier = {}};
        case glz::error_code::constraint_violated:
            return {.kind = ErrorKind::CONSTRAINT, .identifier = {}};
        case glz::error_code::unknown_key:
            return {.kind = ErrorKind::UNKNOWN_KEY, .identifier = {}};
        case glz::error_code::unexpected_enum:
            return {.kind = ErrorKind::ENUM, .identifier = {}};
        default:
            return {.kind = ErrorKind::OTHER, .identifier = glz::format_error(code)};
    }
}

ScanPolicy scan_policy(const ParseFailure& failure) noexcept {
    return {.key_offset_after_quote = failure.kind == ErrorKind::UNKNOWN_KEY,
            .inspect_complete_string = failure.kind == ErrorKind::ENUM};
}

}  // namespace Config::Diagnostic
