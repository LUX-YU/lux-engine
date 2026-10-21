#pragma once

#include <lux/engine/render/features/FeatureBinding.hpp>
#include <lux/engine/render/features/FeaturePorts.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/Sampler.hpp>

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
        std::vector<SamplerDescription>& samplers;

        [[nodiscard]] std::uint32_t addSampler(const SamplerDescription& description) noexcept
        {
            const auto index = static_cast<std::uint32_t>(samplers.size());
            samplers.push_back(description);
            return index;
        }

        [[nodiscard]] ResourceVersion resource(std::string_view key) const noexcept;
        [[nodiscard]] RenderResult<void> publish(std::string_view key, ResourceVersion version) noexcept;
    };
} // namespace lux::render
