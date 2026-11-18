#pragma once

#include <compare>
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>

namespace lux::render
{
    struct CapabilityOutputSupport final
    {
        rdesc::ETextureFormat format{rdesc::ETextureFormat::UNDEFINED};
        std::uint32_t samples{1};
        ETargetKind kind{ETargetKind::OFFSCREEN};

        auto operator<=>(const CapabilityOutputSupport&) const noexcept = default;
    };

    // One implementation can contribute several independent Features to the same Effect.
    // IDs borrow registered graph factories; this record owns no Feature or GPU instance.
    struct CapabilityImplementation final
    {
        std::vector<FeatureId>      features;
        std::string                 implementation;
        std::string                 description;
        // Each entry permits one combination. Sampling uses the separate Target and Graph contracts.
        std::vector<CapabilityOutputSupport> outputs;
        // Exact calls already owned by CommandRegistry, in the implementation's declared method order.
        std::vector<CommandBindingLocation> command_bindings;
    };

} // namespace lux::render
