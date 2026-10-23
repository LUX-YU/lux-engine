#pragma once

#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/Sampler.hpp>
#include <lux/engine/render/resource/Binding.hpp>

namespace lux::render::detail
{
    struct FeatureFrameContribution;
}

namespace lux::render
{
    // Stable cold key for one Feature contribution. No Runtime lookup or registry side effect.
    [[nodiscard]] LUX_RENDER_PUBLIC std::string featureKey(
        FeatureName type,
        std::string_view instance,
        std::string_view local
    ) noexcept;

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

        // Resolves one declared port in this contribution; the key is local to this Feature use.
        [[nodiscard]] ResourceVersion resource(std::string_view key) const noexcept;
        // Connects a produced version to an output port. It creates no Storage or persistent Feature instance.
        [[nodiscard]] RenderResult<void> publish(std::string_view key, ResourceVersion version) noexcept;
    };
} // namespace lux::render
