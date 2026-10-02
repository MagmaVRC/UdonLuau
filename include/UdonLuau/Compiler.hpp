#pragma once

#include "UdonLuau/Catalog.hpp"
#include "UdonLuau/Program.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace UdonLuau {

    enum class Severity : uint8_t {
        Error,
        Warning,
    };

    /// <summary>A compiler message. Lines and columns are zero-based; the end is exclusive.</summary>
    struct Diagnostic {
        Severity    severity = Severity::Error;
        std::string message;
        int         line = 0;
        int         column = 0;
        int         endLine = 0;
        int         endColumn = 0;
    };

    struct CompileResult {
        std::optional<Program>  program;
        std::vector<Diagnostic> diagnostics;

        [[nodiscard]] bool Succeeded() const { return program.has_value(); }
    };

    /// <summary>Compiles one Luau module into one Udon program against the externs the
    /// catalog exposes.</summary>
    CompileResult Compile(const Catalog& catalog, std::string_view source);

} // namespace UdonLuau
