#pragma once

#include <algorithm>
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/features/CapabilityLowering.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>

namespace lux::render
{
    // Effect commands resolve to the sole CommandRegistry. Resource methods belong to their Resource declaration.
    struct CapabilityCommandBinding final
    {
        CommandBindingLocation location;
        bool operator==(const CapabilityCommandBinding&) const noexcept = default;
    };

    // One implementation can contribute several independent Features to the same Effect.
    // IDs borrow registered graph factories; this record owns no Feature or GPU instance.
    struct CapabilityImplementation final
    {
        std::vector<FeatureId>      features;
        std::string                 implementation;
        std::string                 description;
        CapabilityGraphDescription  graph;
        RenderConfigurationFormat   configuration;
        cxx::type_info              config_type;
        // Composition and configuration belong to the implementation, never to a leaf Feature.
        RenderResult<ConfigValue> (*make_default_configuration)() noexcept = nullptr;
        RenderResult<ConfigValue> (*decode_configuration)(std::span<const std::byte>) noexcept = nullptr;
        RenderResult<CapabilityGraphContribution> (*build_graph)(const CapabilityBuildInput&) noexcept = nullptr;
        FrameParameterProjection    project_parameters = nullptr;
        // Exact calls already owned by CommandRegistry, in the implementation's declared method order.
        std::vector<CapabilityCommandBinding> command_bindings;

        struct DrawInput final
        {
            std::uint32_t    graph_feature;
            std::string_view contribution_key;
            std::string_view pass_key;
            bool operator==(const DrawInput&) const noexcept = default;
        };

        std::vector<DrawInput> draw_inputs;
        // View data is projected into execution bytes; it is never an Effect-owned input value.
        cxx::type_info view_parameter_type;

        bool operator==(const CapabilityImplementation& other) const noexcept
        {
            return  features == other.features &&
                    implementation == other.implementation &&
                    description == other.description &&
                    graph == other.graph &&
                    configuration == other.configuration &&
                    config_type == other.config_type &&
                    make_default_configuration == other.make_default_configuration &&
                    decode_configuration == other.decode_configuration &&
                    build_graph == other.build_graph &&
                    project_parameters == other.project_parameters &&
                    command_bindings == other.command_bindings &&
                    draw_inputs == other.draw_inputs &&
                    view_parameter_type == other.view_parameter_type;
        }
    };

} // namespace lux::render
