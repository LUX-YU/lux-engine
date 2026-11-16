#pragma once

#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>

namespace lux::render
{
    // A final observable output from a selected capability occurrence. Internal Feature ports
    // and connections are resolved by the implementation and are not ordinary client input.
    struct CapabilityOutput final
    {
        std::string contribution;
        std::string name;
        bool operator==(const CapabilityOutput&) const noexcept = default;
    };

    struct CapabilityContribution final
    {
        std::string key;
        cxx::type_info capability;
        CapabilityImplementationId implementation_id;
        VCapabilityConfiguration configuration;
    };

    // Cold authoring input only. Lowered into the existing EffectDefinition; it is
    // never retained as a second executable graph or a Feature instance directory.
    struct CapabilityComposition final
    {
        std::vector<CapabilityContribution> contributions;
        std::vector<CapabilityOutput> outputs;
    };

    struct CapabilityContributionDescription final
    {
        std::string key;
        std::string capability;
        std::string implementation;
        SerializedRenderConfiguration configuration;
        bool operator==(const CapabilityContributionDescription&) const noexcept = default;
    };

    // Persistent names/versions, resolved against this Server's capability directory
    // on open. Contains no session IDs, C++ object bytes or GPU resource handles.
    struct CapabilityCompositionDescription final
    {
        std::vector<CapabilityContributionDescription> contributions;
        std::vector<CapabilityOutput> outputs;
        bool operator==(const CapabilityCompositionDescription&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC bool validCapabilityComposition(const CapabilityCompositionDescription&) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<std::vector<std::byte>>
    encodeCapabilityComposition(const CapabilityCompositionDescription&) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<CapabilityCompositionDescription>
    decodeCapabilityComposition(std::span<const std::byte>) noexcept;
} // namespace lux::render
