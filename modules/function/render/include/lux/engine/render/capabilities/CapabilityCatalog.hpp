#pragma once

#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <optional>
#include <span>
#include <vector>

namespace lux::render
{
    // One resolved selectable implementation. This is a query result, never registration storage.
    struct CapabilityImplementationInfo final
    {
        CapabilityImplementationId implementation_id;
        std::string implementation;
        std::string description;
        CapabilityDescription capability;
        std::vector<FeatureId> features;
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

} // namespace lux::render
