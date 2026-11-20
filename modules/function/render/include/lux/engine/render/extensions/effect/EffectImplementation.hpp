#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    class EffectBuildContext;
    class EffectInitializationContext;

    // Registered once during module assembly, immutable after admission. This is
    // the implementation selected by a capability node; it is not another node type.
    struct EffectImplementation final
    {
        using Initialize = RenderResult<void> (*)(EffectInitializationContext&, std::string_view arguments) noexcept;
        using Build = RenderResult<void> (*)(EffectBuildContext&, std::string_view arguments) noexcept;

        graph::NodeTypeId capability;
        std::uint32_t capability_version = 1;
        ImplementationSelection identity;
        std::string display_name;
        std::vector<graph::PinSemanticId> supported_outputs;
        std::vector<CommandBindingLocation> command_bindings;
        Initialize initialize = nullptr;
        Build build = nullptr;
        object::CodeLease code = object::CodeLease::builtin();
    };
}
