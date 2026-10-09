#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/compiler/visibility.h>
#include <lux/engine/material/graph/MaterialGraph.hpp>

namespace lux::material
{
    [[nodiscard]] LUX_ENGINE_MATERIAL_COMPILER_PUBLIC lux::cxx::
        expected<lux::rdesc::MaterialDescription, MaterialCompileFailure>
        compileMaterial(const MaterialGraph& graph) noexcept;
} // namespace lux::material
