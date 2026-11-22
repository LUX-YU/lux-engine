#pragma once

#include <lux/engine/render/effect/UniformBlockVersions.hpp>
#include <lux/engine/render/graph/PreparationPlan.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::compiled
{
    // Published together after all device programs, preparation operands and imports have linked.
    struct RenderGraph final
    {
        detail::RenderGraphExecutable executable;
        detail::NativeProgramBindings programs;
        std::vector<GraphResourceId> target_imports;
        std::vector<GraphResourceId> storage_imports;
        detail::PreparationPlan preparation;
        std::vector<detail::LinkedUniformBlock> uniforms;
    };
} // namespace lux::render::compiled
