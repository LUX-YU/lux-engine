#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>

namespace lux::render::vulkan
{
    class ReadbackOperation;

    class ExecutionContext final
    {
    public:
        ~ExecutionContext() noexcept;
        ExecutionContext(const ExecutionContext&) = delete;
        ExecutionContext& operator=(const ExecutionContext&) = delete;
        [[nodiscard]] VkDevice device() const noexcept { return code_->device(); }
        [[nodiscard]] const CompiledGraph& code() const noexcept { return *code_; }
        [[nodiscard]] const TargetDesc& target() const noexcept { return target_; }
        [[nodiscard]] std::optional<SubmissionTicket> lastUse() const noexcept { return last_use_; }
        [[nodiscard]] RenderResult<void> setContextParameters(std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] std::span<const std::byte> contextParameters() const noexcept { return context_values_; }
        [[nodiscard]] RenderResult<void> setParameters(GraphPassId pass, std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] std::span<const std::byte> parameters(GraphPassId pass) const noexcept;

        // Owner-thread caller has validated the viewport at its request boundary.
        void setViewport(const RasterViewport& viewport) noexcept { viewport_ = viewport; }
        [[nodiscard]] const RasterViewport& viewport() const noexcept { return viewport_; }
        [[nodiscard]] RenderResult<EExecutionOutcome> execute() noexcept;
        [[nodiscard]] const ExecutionProgress& progress() const noexcept { return progress_; }
        [[nodiscard]] RenderResult<bool> completed() noexcept;
        [[nodiscard]] RenderResult<std::vector<std::byte>> readback(std::uint32_t resource = kGraphInvalidIndex) noexcept;
        // Zero means no such explicit exported readback buffer. Drawing does not require one.
        [[nodiscard]] std::size_t readbackBytes(std::uint32_t resource = kGraphInvalidIndex) const noexcept
        {
            if (resource == kGraphInvalidIndex) resource = readback_;
            const bool is_readback = resource < bindings_.size() && code_->resources()[resource].readback;
            return is_readback && bindings_[resource].buffer ? bindings_[resource].buffer->size() : 0;
        }
        [[nodiscard]] StorageSet& attachmentStorages() noexcept { return storages_; }
        template <class Visitor> void forEachStorage(Visitor&& visitor) const noexcept
        {
            for (const auto& use : storage_uses_) visitor(code_->resources()[use.resource].key, use.storage_id);
        }
        // Compiled resource indices are trusted here; this is a Native diagnostic borrow.
        [[nodiscard]] VkBuffer nativeBuffer(std::uint32_t resource) const noexcept
        {
            return bindings_[resource].buffer ? bindings_[resource].buffer->native() : VK_NULL_HANDLE;
        }

    private:
        friend struct NativeMemoryAudit;
        friend class DeviceSession;
        friend class ReadbackOperation;
        void cancelReadbacks() noexcept;
        void withdraw() noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        void rebind() noexcept;
        ReadbackOperation* readbacks_ = nullptr;
        struct Binding final
        {
            Image* image = nullptr;
            VkImageView view{};
            Buffer* buffer = nullptr;
        };
        struct Pass final
        {
            const CompiledPass* recipe;
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
            std::vector<VkDescriptorSet> descriptors;
            std::array<VkDescriptorSet, 2> private_descriptors{};
            std::vector<std::byte> parameters;
            struct ParameterCopy final
            {
                const std::span<const std::byte>* source;
                std::uint32_t source_offset, destination_offset, bytes;
            };
            std::vector<ParameterCopy> parameter_copies;
        };
        ExecutionContext(DeviceSession& owner, SubmissionQueue& queue, std::shared_ptr<const CompiledGraph> code, TargetDesc target) noexcept;
        DeviceSession* owner_;
        SubmissionQueue* queue_;
        struct FrameResources;
        struct PreparedBindings;
        std::shared_ptr<FrameResources> frame_resources_;
        std::shared_ptr<const PreparedBindings> prepared_;
        const CompiledGraph* code_;
        TargetDesc target_;
        RasterViewport viewport_;
        std::vector<Binding> bindings_;
        struct StorageUse final
        {
            StorageId storage_id;
            std::shared_ptr<void> storage_reference;
            PreparedStorageBinding binding;
            std::uint32_t resource;
        };
        StorageSet storages_;
        std::vector<StorageUse> storage_uses_;

        std::vector<Pass> passes_;
        std::vector<std::byte> context_values_;
        std::span<const std::byte> context_view_; // Stable range after prepare, used by compiled parameter copies.

        unsigned descriptor_bank_ = 0;
        unsigned submitted_bank_ = 0;
        std::vector<VkBufferMemoryBarrier2> host_barriers_;
        std::optional<SubmissionTicket> last_use_;
        ExecutionProgress progress_;
        std::uint32_t readback_ = kGraphInvalidIndex;
        std::uint32_t readers_ = 0;

    };
}
