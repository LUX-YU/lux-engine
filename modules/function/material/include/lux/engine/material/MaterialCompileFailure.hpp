#pragma once

#include <lux/engine/function/graph/GraphTypes.hpp>

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
        INVALID_RESULT
    };

    struct MaterialCompileFailure final
    {
        EMaterialCompileError code{EMaterialCompileError::INVALID_GRAPH};
        std::string message;
        lux::graph::NodeId node_id{};
        std::uint32_t pin_index{~std::uint32_t{0}};
    };
} // namespace lux::material
