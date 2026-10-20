#pragma once

#include <lux/engine/render/features/FeatureBinding.hpp>
#include <lux/engine/render/features/FeaturePorts.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>

namespace lux::render
{
    // Borrowed for one synchronous contribution. Graph owns programs and Pass work;
    // Effect owns the separate Attach materials. No device or Runtime access.
    struct LUX_RENDER_PUBLIC FeatureBuildContext final
    {
        RenderGraphBuilder& graph;
        std::span<ResolvedResource> ports;
        std::vector<StorageArgument>& arguments;
        std::vector<detail::FeatureFrameContribution>& bindings;

        [[nodiscard]] ResourceVersion resource(std::string_view key) const noexcept;
        [[nodiscard]] RenderResult<void> publish(std::string_view key, ResourceVersion version) noexcept;
    };
} // namespace lux::render
