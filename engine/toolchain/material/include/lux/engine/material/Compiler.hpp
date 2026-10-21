#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/compiler/visibility.h>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <span>

namespace lux::material
{
    using MaterialCompileResult = cxx::expected<rdesc::MaterialDescription, MaterialCompileFailure>;

    // Compile only the requested uses. Forward supports Unlit; GBuffer supports Unlit/PBR.
    // Shadow evaluates coverage only. The request must be nonempty and contain no duplicates.
    [[nodiscard]] LUX_ENGINE_MATERIAL_COMPILER_PUBLIC MaterialCompileResult
    compileMaterial(const MaterialGraph& graph, std::span<const rdesc::EMaterialPurpose> purposes) noexcept;
} // namespace lux::material
