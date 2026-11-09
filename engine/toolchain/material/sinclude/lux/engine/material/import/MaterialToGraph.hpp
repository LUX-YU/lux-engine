#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/ImportedMaterialDescription.hpp>
#include <string>

namespace lux::material::compiler
{
    // Shared cooker operation. Fixed topology and semantic texture slots; values stay in parameter defaults.
    [[nodiscard]] cxx::expected<MaterialGraph, std::string>
    materialToGraph(const ImportedMaterialDescription& description);
}
