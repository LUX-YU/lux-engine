#pragma once

#include <lux/engine/render/runtime/Backend.hpp>
#include <lux/engine/render/vulkan/graph/Executable.hpp>

namespace lux::render::vulkan
{
    struct BackendOptions
    {
        InstanceOptions instance;
        DeviceOptions device;
        RuntimeCapacity capacity;
    };

    struct ProgramAssignment
    {
        std::string field_path;
        ShaderOwnerId owner;
        bool operator==(const ProgramAssignment&) const noexcept = default;
    };

    struct ProgramBindings
    {
        std::vector<ProgramAssignment> assignments;
        std::vector<OwnerShape> owners;
        bool operator==(const ProgramBindings&) const noexcept = default;
    };

    // A cold compiler input, with no Native objects. Schema spans are synchronous
    // borrows for addEffect; only complete value identities survive publication.
    struct ProgramSource
    {
        GraphPassId pass;
        toolchain::CompiledShaderVariant shader;
        rdesc::PassShaderContract schema;
        ProgramBindings bindings;
        VPipelineDescription pipeline;
    };

    // Fixed Feature composition builds the complete neutral definition before compilation.
    // Cache comparison precedes all Native Shader/Pipeline/Graph creation.
    struct EffectSource
    {
        RenderGraphDefinition definition;
        std::vector<ProgramSource> programs;
        std::vector<NativePassCommand> commands;
        std::uint32_t width{}, height{};
    };
} // namespace lux::render::vulkan
