#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/visibility.h>

namespace lux::material
{
    class MaterialGraph;

    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC cxx::expected<MaterialIR, MaterialCompileFailure>
    lowerMaterial(const MaterialGraph& graph) noexcept;
} // namespace lux::material
