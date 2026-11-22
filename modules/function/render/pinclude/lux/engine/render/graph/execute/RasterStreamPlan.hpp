#pragma once

#include <lux/engine/render/device/ProgramCompileInput.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    struct RasterStreamTarget final
    {
        std::vector<nri::Format> colors;
        nri::Format depth;
        std::uint32_t samples;
        bool dynamic_depth_bias;
        bool shared_textures;
        bool shared_records;
    };

    struct CompiledRasterStream final
    {
        struct ArgumentUses final
        {
            std::uint32_t first = 0, count = 0;
            bool operator==(const ArgumentUses&) const noexcept = default;
        };

        struct Program final
        {
            nri::Pipeline* pipeline;
            NativeProgramId cache_id;
            PassBindings bindings;
            std::vector<ArgumentUses> argument_to_use;
            std::vector<VertexBinding> vertices;
            std::vector<VertexAttribute> mesh_attributes;
            std::uint32_t shared_texture_stages;
            std::uint32_t record_stages;
            rdesc::ShaderBlock record_layout;

            bool operator==(const Program&) const noexcept = default;
        };

        struct BufferAccess final
        {
            std::uint32_t resource;
            GraphBufferRange range;
            EGraphUse usage;
            bool whole_resource;
            bool dynamic_extent;
        };

        struct ImageAccess final
        {
            std::uint32_t resource;
            GraphImageRange range;
            std::uint32_t fallback;
        };

        struct Sampler final
        {
            nri::Descriptor* descriptor;
            bool comparison;
        };

        struct RuntimeContract final
        {
            std::vector<RasterPassTemplate::Input> arguments;
            RasterStreamTarget target;
        };

        std::vector<Program> programs;
        std::vector<BufferAccess> buffers;
        std::vector<ImageAccess> images;
        std::vector<Sampler> samplers;
        std::vector<EGraphUse> argument_uses;
        std::optional<RuntimeContract> runtime;
    };

    // These declaration fields are borrowed only while linking the stream on the owner.
    struct RasterProgramRecipe final
    {
        std::span<const RasterPassTemplate::Input> arguments;
        const ParameterBinding& parameters;
        const RasterStreamTarget& target;
        const rdesc::ShaderProgramArtifact& artifact;
        const GraphicsState& graphics;
        std::span<const std::uint32_t> geometry_locations;
    };

    struct RasterProgramLink final
    {
        ProgramCompileInput input;
        std::vector<CompiledRasterStream::ArgumentUses> argument_to_use;
        std::vector<VertexBinding> vertices;
        std::vector<VertexAttribute> mesh_attributes;
        std::uint32_t shared_texture_stages;
        std::uint32_t record_stages;
        rdesc::ShaderBlock record_layout;

        [[nodiscard]] CompiledRasterStream::Program finish(PreparedProgram) && noexcept;
    };

    [[nodiscard]] RenderResult<RasterProgramLink>
    linkRasterStreamProgram(const DeviceSession&, RasterProgramRecipe) noexcept;

    struct DynamicRasterPrograms final
    {
        std::uint64_t revision = 0;
        std::vector<CompiledRasterStream::Program> programs;
    };
} // namespace lux::render::detail
