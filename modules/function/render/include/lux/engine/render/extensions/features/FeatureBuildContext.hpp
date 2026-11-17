#pragma once

#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/features/FeaturePorts.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/extensions/graph/Sampler.hpp>
#include <lux/engine/render/extensions/resource/Binding.hpp>

namespace lux::render::detail
{
    struct FeatureFrameContribution;
    struct FeatureBuildAccess;
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
        // Attach-time Resource configuration, qualified against this contribution's declared ports.
        void require(StorageArgument argument) noexcept;

        // Resolves one declared port in this contribution; the key is local to this Feature use.
        [[nodiscard]] ResourceVersion resource(std::string_view key) const noexcept;
        // Connects a produced version to an output port. It creates no Storage or persistent Feature instance.
        [[nodiscard]] RenderResult<void> publish(std::string_view key, ResourceVersion version) noexcept;

    private:
        friend struct detail::FeatureBuildAccess;
        FeatureBuildContext(
            RenderGraphBuilder& builder,
            std::span<ResolvedResource> ports,
            std::vector<StorageArgument>& arguments,
            std::vector<detail::FeatureFrameContribution>& bindings
        ) noexcept : graph(builder), ports_(ports), arguments_(arguments), bindings_(bindings)
        {
        }

        std::span<ResolvedResource> ports_;
        std::vector<StorageArgument>& arguments_;
        std::vector<detail::FeatureFrameContribution>& bindings_;

    };
} // namespace lux::render
