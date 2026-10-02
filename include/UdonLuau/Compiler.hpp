#pragma once

#include "UdonLuau/Catalog.hpp"
#include "UdonLuau/Program.hpp"

#include <map>
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

    /// <summary>Settings that apply to one compilation. A define's value is Luau literal text
    /// ("true", "42", "1.5", or any other text as a string) and is substituted wherever the
    /// name is used, so code behind a false define is never compiled.</summary>
    struct CompileOptions {
        std::map<std::string, std::string, std::less<>> defines;
    };

    struct CompileResult {
        std::optional<Program>    program;
        std::optional<ScriptInfo> scriptInterface;
        std::vector<Diagnostic>   diagnostics;

        [[nodiscard]] bool Succeeded() const { return program.has_value(); }
    };

    /// <summary>Compiles one Luau module into one Udon program against the externs and scripts
    /// the catalog exposes. The result also carries the module's public interface.</summary>
    CompileResult Compile(const Catalog& catalog, std::string_view source, const CompileOptions& options = {});

    /// <summary>Reads a module's public methods and fields without compiling function bodies, so
    /// scripts that reference each other can be registered before any of them is compiled. Script
    /// names it refers to must already be in the catalog, interfaces may still be empty.</summary>
    CompileResult ExtractInterface(const Catalog& catalog, std::string_view source, const CompileOptions& options = {});

} // namespace UdonLuau
