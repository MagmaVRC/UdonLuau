#pragma once

#include "UdonLuau/Catalog.hpp"

#include <string>

namespace UdonLuau {

    /// <summary>Generates a Luau definitions file (.d.luau) describing everything the catalog
    /// exposes, for luau-lsp and other Luau tooling: types with their properties, methods,
    /// operators and constructors, enums, behaviour scripts, and the globals UdonLuau provides.</summary>
    std::string GenerateDefinitions(const Catalog& catalog);

} // namespace UdonLuau
