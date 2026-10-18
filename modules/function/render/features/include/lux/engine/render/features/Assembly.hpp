#pragma once

#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/features/Resources.hpp>
#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/graph/Builder.hpp>

namespace lux::render
{
    // Synchronous cold contributions. Callbacks may append, never finish/move/retain the Builder.
    struct FeatureGraphOps final
    {
        RenderResult<void> (*describe)(const void*, std::string_view, ResourceClaims&) noexcept;
        RenderResult<
            void> (*build)(const void*, std::string_view, std::span<const ResolvedResource>, RenderGraphBuilder&) noexcept =
            nullptr;
    };

    struct FeatureInstanceView final
    {
        const FeatureDescriptor* descriptor = nullptr;
        const FeatureGraphOps* graph = nullptr;
        const ConfigValue* config = nullptr;
        std::string_view instance_key;
    };

    struct FeatureContribution final
    {
        std::size_t input_index;
        std::size_t first_pass;
        std::size_t pass_count;
    };

    struct FeatureAssembly final
    {
        RenderGraphDefinition definition;
        GraphAnalysis analysis;
        GraphStorageBindings external_bindings;
        std::vector<FeatureContribution> contributions;
    };

    [[nodiscard]] std::string featureKey(FeatureName type, std::string_view instance, std::string_view local) noexcept;

    [[nodiscard]] RenderResult<FeatureAssembly> assembleFeatures(
        std::span<const FeatureInstanceView> instances,
        std::size_t config_byte_limit
    ) noexcept;

    // Arguments: canonical feature index, resource/group index, related index.
    inline constexpr auto kFeatureContract = error::errorId("lux.render.feature.contract");
    inline constexpr auto kFeatureDuplicate = error::errorId("lux.render.feature.duplicate");
    inline constexpr auto kFeatureMissingProvider = error::errorId("lux.render.feature.missing_provider");
    inline constexpr auto kFeatureResourceMismatch = error::errorId("lux.render.feature.resource_mismatch");
    inline constexpr auto kFeatureOwnership = error::errorId("lux.render.feature.ownership");
    inline constexpr auto kFeatureVersion = error::errorId("lux.render.feature.version");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept;
} // namespace lux::render
