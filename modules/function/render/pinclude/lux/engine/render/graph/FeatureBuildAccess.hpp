#pragma once

#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>

namespace lux::render::detail
{
    // Assembly-only access to the still-active preparation protocol. Its owner and consumers
    // migrate together with the graph instance; external authors cannot inspect its mutable collectors.
    struct FeatureBuildAccess final
    {
        static FeatureBuildContext
        context(RenderGraphBuilder& graph, std::span<ResolvedResource> ports, FeatureAssembly& assembly) noexcept
        {
            return {graph, ports, assembly.arguments, assembly.bindings};
        }

        static void bind(FeatureBuildContext& context, FeatureFrameContribution contribution) noexcept
        {
            context.bindings_.push_back(std::move(contribution));
        }
    };
} // namespace lux::render::detail
