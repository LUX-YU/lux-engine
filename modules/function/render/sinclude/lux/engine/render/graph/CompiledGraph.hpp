#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/features/FeatureFrameContribution.hpp>
#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <memory>

namespace lux::render::detail
{
    struct LayoutPlan;
    struct EnabledDeviceFeatures;

    struct CompiledResource final
    {
        std::string key;
        VGraphResourceDesc description;
        EGraphOrigin origin;
        std::optional<StorageRequirement> storage;
        nri::TextureUsageBits image_usage = nri::TextureUsageBits::NONE;
        nri::BufferUsageBits buffer_usage = nri::BufferUsageBits::NONE;
        bool used = false;
        bool target_sized = false;
        std::uint32_t context_data = kGraphInvalidIndex;
        // Immutable reverse indexes shared by every Context frame slice.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> barrier_uses, descriptor_uses;
    };

    enum class ERecipe : std::uint8_t
    {
        DRAW,
        DISPATCH,
        CLEAR_IMAGE,
        FILL_BUFFER,
        COPY_IMAGE_BUFFER,
        COPY_BUFFER
    };

    struct ImageBarrier final
    {
        std::uint32_t resource;
        nri::TextureBarrierDesc native;
    };

    struct BufferBarrier final
    {
        std::uint32_t resource;
        nri::BufferBarrierDesc native;
    };

    struct CompiledVertexBuffer final
    {
        std::uint32_t binding, resource;
        std::uint32_t stride;
        std::uint64_t offset;
    };

    enum class EDrawRecipe : std::uint8_t
    {
        VERTICES,
        INDEXED,
        INDEXED_INDIRECT
    };

    // One compiled pass recipe, not an author graph. Native handles belong to CompiledGraph.
    struct CompiledPass final
    {
        ERecipe recipe;
        std::uint32_t source = kGraphInvalidIndex;
        std::uint32_t destination = kGraphInvalidIndex;
        std::uint32_t color = kGraphInvalidIndex, depth = kGraphInvalidIndex;
        nri::LoadOp depth_load = nri::LoadOp::CLEAR;
        float clear_depth = 1;
        std::vector<ImageBarrier> images;
        std::vector<BufferBarrier> buffers;
        nri::Pipeline* pipeline{};
        nri::LoadOp load = nri::LoadOp::CLEAR;
        std::array<float, 4> clear_color{};
        EDrawRecipe draw = EDrawRecipe::VERTICES;
        std::vector<CompiledVertexBuffer> vertices;
        std::uint32_t index_resource = kGraphInvalidIndex, indirect_resource = kGraphInvalidIndex;
        nri::IndexType index_type = nri::IndexType::UINT32;
        std::uint64_t index_offset = 0, indirect_offset = 0;
        std::uint32_t count = 0, instances = 1, first = 0, first_instance = 0, indirect_stride = 0;
        std::int32_t vertex_offset = 0;
        std::array<std::uint32_t, 3> dispatch{};
        bool begin_rendering = true;
        bool end_rendering = true;
        bool bind_pipeline = true;
        std::uint32_t layout_program = kGraphInvalidIndex; // cold LayoutPlan entry, not a second graph
    };

    // The Effect owns this immutable result; execution contexts borrow it within the Effect lifetime.
    class LUX_RENDER_PUBLIC CompiledGraph final
    {
    public:
        ~CompiledGraph() noexcept;
        CompiledGraph(CompiledGraph&& other) noexcept;
        CompiledGraph& operator=(CompiledGraph&&) = delete;
        CompiledGraph(const CompiledGraph&) = delete;
        CompiledGraph& operator=(const CompiledGraph&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] nri::PipelineLayout* layout() const noexcept
        {
            return layout_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] std::span<const CompiledResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] const StorageGroups<std::uint32_t>& storageGroups() const noexcept
        {
            return storage_groups_;
        }

        [[nodiscard]] std::span<const CompiledPass> passes() const noexcept
        {
            return passes_;
        }

        [[nodiscard]] std::uint32_t output() const noexcept
        {
            return output_;
        }

        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

    private:
        friend class ExecutionContext;
        friend LUX_RENDER_PUBLIC RenderResult<CompiledGraph>
        compileGraph(
            nri::Device&,
            const nri::CoreInterface&,
            const EnabledDeviceFeatures&,
            std::span<const nri::FormatSupportBits>,
            std::array<std::uint32_t, 2>,
            nri::QueueType,
            std::uint32_t,
            std::uint32_t,
            const RenderGraphDefinition&,
            const GraphAnalysis&,
            GraphStorageBindings,
            std::span<const PassProgram* const>,
            const TargetDesc&,
            std::vector<FeatureFrameContribution>
        ) noexcept;

        CompiledGraph(nri::Device& device, const nri::CoreInterface&, TargetDesc target) noexcept;
        std::unique_ptr<LayoutPlan> descriptor_plan_;
        nri::Device* device_;
        const nri::CoreInterface* nri_core_;
        TargetDesc target_;
        nri::PipelineLayout* layout_{};
        std::vector<CompiledResource> resources_;
        std::vector<FeatureFrameContribution> frame_contributions_;
        StorageGroups<std::uint32_t> storage_groups_;
        std::vector<CompiledPass> passes_;
        std::vector<std::uint32_t> pass_indices_;

        std::vector<nri::Pipeline*> pipelines_; // unique physical owners; Pass recipes only borrow handles
        std::uint32_t output_ = kGraphInvalidIndex;
        bool resizable_output_ = false;
    };

} // namespace lux::render::detail
