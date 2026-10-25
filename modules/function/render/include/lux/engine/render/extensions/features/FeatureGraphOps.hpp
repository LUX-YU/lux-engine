#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <string_view>

namespace lux::render
{
    struct FeatureBuildContext;
    struct FeaturePorts;
    struct EffectDefinition;
    struct FeatureNameTag;
    using FeatureName = cxx::SchemaIdView<FeatureNameTag>;

    enum class ECapabilityInputScope : std::uint8_t
    {
        GLOBAL,
        SCENE,
        VIEW,
        ATTACHMENT
    };

    // An implementation's mapping from a public input schema to its actual professional storage.
    // StorageSet owns the instance; the mapping owns neither a resource nor a scope.
    struct CapabilityStorageInput final
    {
        std::string_view schema;
        std::uint32_t version = 1;
        ECapabilityInputScope scope = ECapabilityInputScope::GLOBAL;
        StorageNameView storage_type;
        std::string_view storage_key;
        // Optional cold projection from Feature configuration to this dependency's concrete configuration.
        // The returned value is adopted as immutable Attach input, never a second Storage owner.
        RenderResult<ConfigValue> (*configuration)(const void*) noexcept = nullptr;
    };

    // Synchronous cold contributions. Callbacks may append, never finish/move/retain the Builder.
    struct FeatureGraphOps final
    {
        using Build = RenderResult<void> (*)(const void*, std::string_view, FeatureBuildContext&) noexcept;
        using Describe = RenderResult<void> (*)(const void*, std::string_view, FeaturePorts&) noexcept;
        using Expand = RenderResult<
            void> (*)(const TargetDesc&, const void*, const void*, std::span<const FeatureId>, EffectDefinition&) noexcept;
        using Parameters =
            RenderResult<void> (*)(cxx::TypeToken, std::span<const std::byte>, std::span<std::byte>) noexcept;

        Describe describe_ports = nullptr;
        Build build = nullptr;
        // A composition expands to the same authoring graph; it has no second execution plan.
        Expand expand = nullptr;
        std::span<const FeatureName> dependencies;
        cxx::TypeToken effect_input_type;
        // Validate the complete input/output contract before writing. Failure must leave output unchanged.
        // Projection writes the instance's existing parameter memory, with no temporary heap buffer.
        Parameters parameters = nullptr;
        std::span<const CapabilityStorageInput> storage_inputs;
    };

    // Arguments: canonical feature index, resource/group index, related index.
    inline constexpr auto kFeatureContract = error::errorId("lux.render.feature.contract");
    inline constexpr auto kFeatureDuplicate = error::errorId("lux.render.feature.duplicate");
    inline constexpr auto kFeatureMissingProvider = error::errorId("lux.render.feature.missing_provider");
    inline constexpr auto kFeatureResourceMismatch = error::errorId("lux.render.feature.resource_mismatch");
    inline constexpr auto kFeatureOwnership = error::errorId("lux.render.feature.ownership");
    inline constexpr auto kFeatureVersion = error::errorId("lux.render.feature.version");
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept;
} // namespace lux::render
