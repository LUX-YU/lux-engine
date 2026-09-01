#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/compiler/visibility.h>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <cstdint>
#include <string>

namespace lux::material
{
    enum class EMaterialCompileError : std::uint8_t
    {
        INVALID_GRAPH,
        CYCLE,
        TYPE_MISMATCH,
        MISSING_REQUIRED_OUTPUT,
        LOWERING_FAILURE,
        SHADER_EMISSION_FAILURE,
        SHADER_COMPILATION_FAILURE,
        INVALID_RESULT,
        ALLOCATION_FAILURE
    };

    struct MaterialCompileFailure final
    {
        EMaterialCompileError code{EMaterialCompileError::INVALID_GRAPH};
        std::string message;
        std::uint64_t node_id{invalid_node};
        std::uint32_t pin_index{invalid_pin};
    };

    [[nodiscard]] LUX_ENGINE_MATERIAL_COMPILER_PUBLIC lux::cxx::expected<
        lux::rdesc::MaterialDescription,
        MaterialCompileFailure
    > compileMaterial(const MaterialGraph& graph) noexcept;
} // namespace lux::material
