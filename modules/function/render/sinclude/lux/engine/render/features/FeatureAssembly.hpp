#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/features/EffectDefinition.hpp>
#include <lux/engine/render/features/FeatureBuildContext.hpp>
#include <lux/engine/render/features/FeatureDescriptor.hpp>
#include <lux/engine/render/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>

namespace lux::render
{
    // Synchronous cold contributions. Callbacks may append, never finish/move/retain the Builder.
    struct FeatureGraphOps final
    {
        using Build = RenderResult<void> (*)(const void*, std::string_view, FeatureBuildContext&) noexcept;

        Build build = nullptr;
    };

    struct FeatureBuildInput final
    {
        const FeatureDescriptor* descriptor = nullptr;
        const FeatureGraphOps* graph = nullptr;
        const ConfigValue* config = nullptr;
        std::string_view contribution_key;
    };

    struct FeatureContribution final
    {
        std::size_t input_index;
        std::size_t first_pass;
        std::size_t pass_count;
    };

    struct FeatureAssembly final
    {
        RenderGraph definition;
        RenderGraphAnalysis analysis;
        GraphStorageBindings external_bindings;
        std::vector<FeatureContribution> contributions;
        std::vector<StorageArgument> arguments;
        std::vector<detail::FeatureFrameContribution> bindings;
        std::vector<SamplerDescription> samplers;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC std::string featureKey(
        FeatureName type,
        std::string_view instance,
        std::string_view local
    ) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<FeatureAssembly> composeFeatures(
        std::span<const FeatureBuildInput> instances,
        std::size_t config_byte_limit,
        std::span<const FeatureConnection> connections,
        std::span<const FeatureExport> outputs,
        std::span<const FeatureInput> inputs = {}
    ) noexcept;

    // Arguments: canonical feature index, resource/group index, related index.
    inline constexpr auto kFeatureContract = error::errorId("lux.render.feature.contract");
    inline constexpr auto kFeatureDuplicate = error::errorId("lux.render.feature.duplicate");
    inline constexpr auto kFeatureMissingProvider = error::errorId("lux.render.feature.missing_provider");
    inline constexpr auto kFeatureResourceMismatch = error::errorId("lux.render.feature.resource_mismatch");
    inline constexpr auto kFeatureOwnership = error::errorId("lux.render.feature.ownership");
    inline constexpr auto kFeatureVersion = error::errorId("lux.render.feature.version");
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept;
} // namespace lux::render
