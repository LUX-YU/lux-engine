#pragma once

#include <lux/engine/render/features/Resources.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    struct FeatureTypeTag;
    using FeatureTypeIdView = cxx::SchemaIdView<FeatureTypeTag>;

    // Metadata and code are immutable and borrowed for the synchronous assembly.
    // Callbacks may append to, but never finish/move/reset/retain, the Builder.
    struct FeatureDefinition final
    {
        FeatureTypeIdView type;
        cxx::SchemaVersion version;
        const DataSchemaDescriptor* config_schema = nullptr;
        RenderResult<void> (*describe)(
            const WireView*, std::string_view, std::vector<ResourceClaim>&
        ) noexcept = nullptr;
        RenderResult<void> (*build)(
            const WireView*, std::string_view, std::span<const ResolvedResource>, RenderGraphBuilder&
        ) noexcept = nullptr;
        std::span<const CommandDescriptor> commands;
    };

    struct SelectedFeature final
    {
        const FeatureDefinition* definition = nullptr;
        const WireView* config = nullptr;
        std::string_view instance_key;
    };

    // Authorship established by Builder append intervals. selection_index refers to the
    // caller's selection order; no Definition/configuration/name storage is borrowed.
    struct FeatureContribution final
    {
        std::size_t selection_index;
        std::size_t first_pass;
        std::size_t pass_count;
    };

    struct FeatureAssembly final
    {
        RenderGraphDefinition definition;
        GraphAnalysis analysis;
        std::vector<ResolvedStorageBinding> external_bindings;
        std::vector<FeatureContribution> contributions;
    };

    // Collision-free private Pass/Resource key. local must be nonempty.
    [[nodiscard]] std::string
    featureKey(FeatureTypeIdView type, std::string_view instance, std::string_view local) noexcept;

    [[nodiscard]] RenderResult<FeatureAssembly>
    assembleFeatures(std::span<const SelectedFeature> selected, WireLimits config_limits) noexcept;

    // Arguments: canonical feature index, resource/group index, related index.
    inline constexpr auto kFeatureContract = error::errorId("lux.render.feature.contract");
    inline constexpr auto kFeatureDuplicate = error::errorId("lux.render.feature.duplicate");
    inline constexpr auto kFeatureMissingProvider = error::errorId("lux.render.feature.missing_provider");
    inline constexpr auto kFeatureResourceMismatch = error::errorId("lux.render.feature.resource_mismatch");
    inline constexpr auto kFeatureOwnership = error::errorId("lux.render.feature.ownership");
    inline constexpr auto kFeatureVersion = error::errorId("lux.render.feature.version");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept;
}
