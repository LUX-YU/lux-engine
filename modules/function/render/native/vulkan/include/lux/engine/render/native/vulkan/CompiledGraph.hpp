#pragma once

#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <memory>
#include <vulkan/vulkan.h>

namespace lux::render::vulkan
{
    struct LayoutPlan;
    class DeviceSession;

    struct NativeResourcePlan final
    {
        std::string key;
        VGraphResourceDesc description;
        EGraphOrigin origin;
        std::optional<StorageRequirement> storage;
        VkImageUsageFlags image_usage = 0;
        VkBufferUsageFlags buffer_usage = 0;
        bool target_sized = false;
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
        VkImageMemoryBarrier2 native;
    };

    struct BufferBarrier final
    {
        std::uint32_t resource;
        VkBufferMemoryBarrier2 native;
    };

    struct CompiledVertexBuffer final
    {
        std::uint32_t binding, resource;
        VkDeviceSize offset;
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
        VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        float clear_depth = 1;
        std::vector<ImageBarrier> images;
        std::vector<BufferBarrier> buffers;
        VkPipeline pipeline{};
        VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        std::array<float, 4> clear_color{};
        EDrawRecipe draw = EDrawRecipe::VERTICES;
        std::vector<CompiledVertexBuffer> vertices;
        std::uint32_t index_resource = kGraphInvalidIndex, indirect_resource = kGraphInvalidIndex;
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        VkDeviceSize index_offset = 0, indirect_offset = 0;
        std::uint32_t count = 0, instances = 1, first = 0, first_instance = 0, indirect_stride = 0;
        std::int32_t vertex_offset = 0;
        std::array<std::uint32_t, 3> dispatch{};
        bool begin_rendering = true;
        bool end_rendering = true;
        bool bind_pipeline = true;
        std::uint32_t layout_program = kGraphInvalidIndex; // cold LayoutPlan entry, not a second graph
    };

    // The Effect owns this immutable result; execution contexts borrow it within the Effect lifetime.
    class CompiledGraph final
    {
    public:
        ~CompiledGraph() noexcept;
        CompiledGraph(CompiledGraph&& other) noexcept;
        CompiledGraph& operator=(CompiledGraph&&) = delete;
        CompiledGraph(const CompiledGraph&) = delete;
        CompiledGraph& operator=(const CompiledGraph&) = delete;

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkPipelineLayout layout() const noexcept
        {
            return layout_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] std::span<const NativeResourcePlan> resources() const noexcept
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
        friend class DeviceSession;
        friend RenderResult<CompiledGraph>
        compileGraph(const DeviceSession&, const RenderGraphDefinition&, const GraphAnalysis&, const GraphStorageBindings&, std::span<const PassProgram>, const TargetDesc&) noexcept;
        CompiledGraph(VkDevice device, TargetDesc target) noexcept;
        std::unique_ptr<LayoutPlan> descriptor_plan_;
        VkDevice device_;
        TargetDesc target_;
        VkPipelineLayout layout_{};
        std::vector<NativeResourcePlan> resources_;
        StorageGroups<std::uint32_t> storage_groups_;
        std::vector<CompiledPass> passes_;
        std::vector<std::uint32_t> pass_indices_;

        struct PipelineIdentity final
        {
            SpirvArtifact shader;
            GraphicsState graphics;
            VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
            bool operator==(const PipelineIdentity&) const noexcept = default;
        };

        std::vector<PipelineIdentity> shader_identities_;
        std::vector<VkPipeline> pipelines_; // unique physical owners; Pass recipes only borrow handles
        std::vector<VkDescriptorSetLayout> descriptor_layouts_;
        std::uint32_t output_ = kGraphInvalidIndex;
    };

    // INVARIANT: analysis belongs to this immutable definition; never analyze again here.
    // Program/Target/device capabilities are the cold BOUNDARY checked by this compiler.
    [[nodiscard]] RenderResult<CompiledGraph> compileGraph(
        const DeviceSession& session,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        const GraphStorageBindings& external_bindings,
        std::span<const PassProgram> programs,
        const TargetDesc& target
    ) noexcept;
} // namespace lux::render::vulkan
