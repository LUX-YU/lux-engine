#pragma once

#include <exec/task.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
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
        bool shared_materials;
    };

    struct CompiledRasterStream final
    {
        struct Program final
        {
            nri::Pipeline* pipeline;
            PassBindings bindings;
            std::vector<VertexBinding> vertices;
            std::vector<VertexAttribute> mesh_attributes;
            std::uint32_t shared_texture_stages;
            std::uint32_t material_stages;
            rdesc::ShaderBlock material_layout;

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

    struct PreparedRasterStreamProgram final
    {
        CompiledRasterStream::Program data;
        CompiledPrograms::Program* lease;
    };

    [[nodiscard]] exec::task<RenderResult<PreparedRasterStreamProgram>>
    prepareRasterStreamProgram(
        DeviceSession&, execution::CpuScheduler, const RasterPassTemplate&, const RasterStreamTarget&,
        const rdesc::ShaderProgramArtifact&, const GraphicsState&,
        std::optional<cxx::function_ref<bool() noexcept>> runtime_current = {}
    ) noexcept;

    struct DynamicRasterPrograms final
    {
        std::uint64_t revision = 0;
        std::vector<CompiledRasterStream::Program> programs;
        std::vector<CompiledPrograms::Program*> leases;
    };
} // namespace lux::render::detail
