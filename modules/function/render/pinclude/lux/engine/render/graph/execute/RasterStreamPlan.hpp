#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    inline constexpr auto kDescriptorPoolCounts = std::array{
        &nri::DescriptorPoolDesc::descriptorSetMaxNum, &nri::DescriptorPoolDesc::samplerMaxNum,
        &nri::DescriptorPoolDesc::constantBufferMaxNum, &nri::DescriptorPoolDesc::storageStructuredBufferMaxNum,
        &nri::DescriptorPoolDesc::storageBufferMaxNum, &nri::DescriptorPoolDesc::textureMaxNum,
        &nri::DescriptorPoolDesc::storageTextureMaxNum, &nri::DescriptorPoolDesc::structuredBufferMaxNum
    };

    struct RasterDescriptorField final
    {
        std::uint32_t binding, element, block_size;
        EDescriptorKind kind;
        FixedDescriptorBinding view;
        bool image_depth, comparison_sampler;
        bool operator==(const RasterDescriptorField&) const noexcept = default;
    };

    struct RasterDescriptorLayout final
    {
        PassDescriptorBindings native;
        std::vector<RasterDescriptorField> fields;
        std::array<std::uint32_t, kDescriptorPoolCounts.size()> per_set{};
        std::uint32_t parameter_bytes = 0;

        [[nodiscard]] RenderResult<void> reserveSet(nri::DescriptorPoolDesc&) const noexcept;
        bool operator==(const RasterDescriptorLayout&) const noexcept = default;
    };

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
            RasterDescriptorLayout bindings;
            std::vector<ArgumentUses> argument_to_use;
            std::vector<VertexBinding> vertices;
            std::vector<VertexAttribute> geometry_attributes;
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

        std::uint32_t list;
        std::vector<Program> programs;
        std::vector<BufferAccess> buffers;
        std::vector<ImageAccess> images;
        std::vector<Sampler> samplers;
        std::vector<EGraphUse> argument_uses;
        std::optional<RuntimeContract> runtime;
    };

    struct RasterProgramSelection final
    {
        std::span<const CompiledRasterStream::Program> programs;
        std::uint64_t revision;
    };

    struct DynamicRasterPrograms final
    {
        std::uint64_t revision = 0;
        std::vector<CompiledRasterStream::Program> programs;
    };
} // namespace lux::render::detail
