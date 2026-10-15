#pragma once

#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/native/vulkan/Program.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::vulkan
{
    struct ResourcePlan final
    {
        std::string key;
        VGraphResourceDesc description;
        EGraphOrigin origin;
        std::optional<StorageRequirement> storage;
        VkImageUsageFlags image_usage = 0;
        VkBufferUsageFlags buffer_usage = 0;
        bool readback = false;
        bool target_sized = false;
        TextureId texture{};
    };

    enum class ERecipe : std::uint8_t { DRAW, CLEAR_IMAGE, FILL_BUFFER, COPY_IMAGE_BUFFER, COPY_BUFFER };

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

    // One compiled pass recipe, not an author graph. Native handles belong to CompiledGraph.
    struct CompiledPass final
    {
        ERecipe recipe;
        std::uint32_t source = kGraphInvalidIndex;
        std::uint32_t destination = kGraphInvalidIndex;
        std::vector<ImageBarrier> images;
        std::vector<BufferBarrier> buffers;
        VkPipeline pipeline{};
        VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        std::array<float, 4> clear_color{};
        std::uint32_t vertex_count = 0;
        bool begin_rendering = true;
        bool end_rendering = true;
        bool bind_pipeline = true;
        float texture_lod = 0;
    };

    // The Effect owns this move-only immutable result; execution contexts borrow/pin that one result.
    class CompiledGraph final
    {
    public:
        ~CompiledGraph() noexcept;
        CompiledGraph(CompiledGraph&& other) noexcept;
        CompiledGraph& operator=(CompiledGraph&&) = delete;
        CompiledGraph(const CompiledGraph&) = delete;
        CompiledGraph& operator=(const CompiledGraph&) = delete;

        [[nodiscard]] VkDevice device() const noexcept { return device_; }
        [[nodiscard]] VkPipelineLayout layout() const noexcept { return layout_; }
        [[nodiscard]] const TargetDesc& target() const noexcept { return target_; }
        [[nodiscard]] std::span<const ResourcePlan> resources() const noexcept { return resources_; }
        [[nodiscard]] std::span<const CompiledPass> passes() const noexcept { return passes_; }
        [[nodiscard]] std::span<const BufferBarrier> hostBarriers() const noexcept { return host_barriers_; }
        [[nodiscard]] std::uint32_t output() const noexcept { return output_; }
        [[nodiscard]] bool compatible(const TargetDesc& target) const noexcept;

    private:
        friend RenderResult<CompiledGraph> compileGraph(
            const DeviceSession&, const FeatureAssembly&, std::span<const PassProgram>, const TargetDesc&
        ) noexcept;
        CompiledGraph(VkDevice device, TargetDesc target) noexcept : device_(device), target_(target) {}
        VkDevice device_;
        TargetDesc target_;
        VkPipelineLayout layout_{};
        std::vector<ResourcePlan> resources_;
        std::vector<CompiledPass> passes_;
        std::vector<BufferBarrier> host_barriers_;
        std::vector<ShaderArtifact> shader_identities_;
        std::vector<VkPipeline> pipelines_; // unique physical owners; Pass recipes only borrow handles
        std::uint32_t output_ = kGraphInvalidIndex;
    };

    // INVARIANT: assembly is the exact successful G5 result; reuse its Analysis, never analyze again.
    // Program/Target/device capabilities are the cold BOUNDARY checked by this compiler.
    [[nodiscard]] RenderResult<CompiledGraph> compileGraph(
        const DeviceSession& session,
        const FeatureAssembly& assembly,
        std::span<const PassProgram> programs,
        const TargetDesc& target
    ) noexcept;
}
