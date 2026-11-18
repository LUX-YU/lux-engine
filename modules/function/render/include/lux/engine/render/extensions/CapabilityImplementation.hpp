#pragma once

#include <algorithm>
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>

namespace lux::render
{
    // One implementation can contribute several independent Features to the same Effect.
    // IDs borrow registered graph factories; this record owns no Feature or GPU instance.
    struct CapabilityImplementation final
    {
        std::vector<FeatureId>      features;
        std::string                 implementation;
        std::string                 description;
        // Device/output eligibility only; no ports, config decoder or graph factory.
        std::vector<rdesc::ETextureFormat> target_formats;
        std::vector<std::uint32_t> target_samples;
        std::vector<ETargetKind> target_kinds;
        // Exact calls already owned by CommandRegistry, in the implementation's declared method order.
        std::vector<CommandBindingLocation> command_bindings;


    };

} // namespace lux::render
