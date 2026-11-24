#pragma once

#include <lux/engine/render/effect/FrameAlgorithm.hpp>
#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <memory>

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
        bool operator==(const RasterStreamTarget&) const noexcept = default;
    };

    struct CompiledRasterProgram final
    {
        struct ArgumentUses final
        {
            std::uint32_t first = 0, count = 0;
            bool operator==(const ArgumentUses&) const noexcept = default;
        };

        nri::Pipeline* pipeline;
        NativeProgramId cache_id;
        RasterDescriptorLayout bindings;
        std::vector<ArgumentUses> argument_to_use;
        std::vector<VertexBinding> vertices;
        std::vector<VertexAttribute> geometry_attributes;
        std::uint32_t shared_texture_stages;
        std::uint32_t record_stages;
        rdesc::ShaderBlock record_layout;

        bool operator==(const CompiledRasterProgram&) const noexcept = default;
    };

    struct CompiledRasterStream final
    {
        [[nodiscard]] RasterOperands
        operands(GraphPassId, DrawStreamSlot, const RasterPassTemplate&, std::uint32_t first_source) const & noexcept;

        struct BufferAccess final
        {
            std::uint32_t resource;
            GraphBufferRange range;
            EGraphUse usage;
            bool whole_resource;
            bool dynamic_extent;
            std::uint32_t group_count = 0;
        };

        struct ImageAccess final
        {
            std::uint32_t resource;
            GraphImageRange range;
            std::uint32_t fallback;
            std::uint32_t group_count = 0;
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
            bool operator==(const RuntimeContract&) const noexcept = default;
        };

        std::uint32_t pass, list, batch = kGraphInvalidIndex;
        bool resource_list = false;

        // Resource lists address this pass directly. Cold-linked algorithm outputs carry a compiled pass ID.
        bool acceptsStream(std::uint32_t owner) const noexcept
        {
            return owner == (resource_list ? kGraphInvalidIndex : pass);
        }

        std::vector<CompiledRasterProgram> programs;
        std::vector<BufferAccess> buffers;
        std::vector<ImageAccess> images;
        std::vector<Sampler> samplers;
        std::vector<EGraphUse> argument_uses;
        struct TargetArgument final
        {
            rdesc::ETextureFormat format;
            nri::StageBits stages;
        };
        std::vector<TargetArgument> target_arguments;
        std::optional<RuntimeContract> runtime;
    };

    // Publications and independently bound Jobs can drop their references in either order.
    // Only linked CPU data is shared. Native pipeline retirement still uses the accepted queue frontiers.
    using PublishedRasterProgram = std::shared_ptr<const CompiledRasterProgram>;

    [[nodiscard]] inline bool
    sameRasterProgram(const PublishedRasterProgram& left, const PublishedRasterProgram& right) noexcept
    {
        return left == right || *left == *right;
    }

    struct RasterProgramSelection final
    {
        std::span<const CompiledRasterProgram> fixed;
        std::span<const PublishedRasterProgram> published;
        std::uint64_t revision;

        [[nodiscard]] std::size_t size() const noexcept { return revision ? published.size() : fixed.size(); }
        [[nodiscard]] const CompiledRasterProgram& operator[](std::size_t index) const noexcept
        {
            return revision ? *published[index] : fixed[index];
        }
    };
} // namespace lux::render::detail
