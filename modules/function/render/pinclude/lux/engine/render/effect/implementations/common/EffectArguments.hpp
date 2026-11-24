#pragma once

#include <lux/cxx/arguments/TypedOptions.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>

namespace lux::render::detail
{
    [[nodiscard]] inline RenderError effectArgumentError(graph::NodeId node, cxx::args::Error error) noexcept
    {
        return {kInvalidEffectArguments, {node.value, static_cast<std::uint64_t>(error.code), error.position}};
    }
}
