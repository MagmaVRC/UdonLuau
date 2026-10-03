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
        /// <summary>The script's name. Needed when the module declares statics, which live in a companion singleton named after it.</summary>
        std::string scriptName;
        /// <summary>Compile the module's static declarations as its companion singleton instead of the per-instance program.</summary>
        bool staticPart = false;
        /// <summary>Begin every entry point with UdonSharp's 0xFFFFFFFF exit marker and leave through the return trampoline, so tools that expect UdonSharp's layout can read the program. Costs a few instructions per event.</summary>
        bool compatibleExitReturn = false;
    };

    /// <summary>The name of the companion singleton that holds a script's static fields and functions.</summary>
    inline std::string StaticCompanionName(std::string_view scriptName) { return std::string(scriptName) + ".Static"; }

    struct CompileResult {
        std::optional<Program>    program;
        std::optional<ScriptInfo> scriptInterface;
        std::vector<Diagnostic>   diagnostics;
        /// <summary>Whether the module declares static fields or functions, so the host must also compile and place its companion.</summary>
        bool                      hasStatics = false;

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
