#pragma once

#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RenderScope.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/frame/RenderFrame.hpp>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lux::render
{
    // File encoding identity only. Command/PerFrameData declarations own their own
    // data contracts; this does not describe resources or runtime borrowing.
    struct RenderConfigurationFormat final
    {
        std::string schema;
        std::uint32_t version = 1;
        bool operator==(const RenderConfigurationFormat&) const noexcept = default;
    };

    // Portable configuration from a saved project. Bytes are decoded by the selected implementation;
    // they are never reinterpreted as a C++ object. The typed alternative remains available in-process.
    struct SerializedRenderConfiguration final
    {
        std::string schema;
        std::uint32_t version = 1;
        std::vector<std::byte> bytes;
        bool operator==(const SerializedRenderConfiguration&) const noexcept = default;
    };

    using VCapabilityConfiguration = std::variant<ConfigValue, SerializedRenderConfiguration>;

    // One resolved selectable implementation. This is a query result, never registration storage.
    struct CapabilityImplementationInfo final
    {
        CapabilityImplementationId implementation_id;
        std::string implementation;
        std::string description;
        CapabilityDescription capability;
        std::vector<std::string> outputs; // Logical final outputs; no graph layout or resource schema.
        RenderConfigurationFormat configuration;
        cxx::type_info config_type;
        bool portable_configuration = false; // Derived from the implementation codec, not promised by its C++ type.
        RenderError unavailable;
    };

    struct CapabilityQuery final
    {
        std::string capability;           // Empty lists all selectable implementations.
        std::string implementation;       // Explicit stable selection, never registration-order fallback.
        std::optional<EffectOutputDesc> output; // Unsupported choices remain visible with an unavailable reason.
    };

    struct CapabilityList final
    {
        RenderError error;
        std::vector<CapabilityImplementationInfo> implementations;
    };

    struct CapabilityConfiguration final
    {
        CapabilityImplementationId implementation_id;
        VCapabilityConfiguration configuration;
    };

} // namespace lux::render
