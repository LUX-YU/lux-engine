#pragma once
#include <lux/engine/core/execution/Scheduler.hpp>

#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class DescriptorArray;
    class ExecutionContext;
    class Transfer;
    class StagingArena;
    struct ImageTransferRegion;
    struct TransferStats;
    // Stable Native lifetime root. Bootstrap owns InstanceContext and outlives this object.
    // Queue roles that alias the same VkQueue borrow the same SubmissionQueue owner.
    class DeviceSession final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<DeviceSession>> create(
            const InstanceContext& instance,
            const DeviceOptions& options = {},
            std::uint32_t slots = 3
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] RenderResult<void> drain() noexcept;
        [[nodiscard]] RenderResult<void> collect() noexcept;
        // Explicit RenderThread safe point, separate from polling and resource admission/collection.
        void dispatchCompletions() noexcept;
        [[nodiscard]] RenderResult<ExecutionContext*> prepare(
            std::shared_ptr<const CompiledGraph> code, const TargetDesc& target,
            StorageSet* scene = nullptr, StorageSet* view = nullptr, std::span<const StorageArgument> arguments = {}
        ) noexcept;
        [[nodiscard]] RenderResult<void> release(ExecutionContext* context) noexcept;
        [[nodiscard]] SubmissionQueue& queue(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *queues_[static_cast<std::size_t>(role)];
        }

        [[nodiscard]] RenderResult<std::uint32_t> sampledImageCapacity() const noexcept;
        [[nodiscard]] RenderResult<std::shared_ptr<DescriptorArray>> sampledImageArray() noexcept;
        [[nodiscard]] VulkanAllocator& allocator() noexcept { return allocator_; }
        [[nodiscard]] const VulkanDevice& device() const noexcept { return device_; }
        // Boundary lookup compares stable owner identity before inspecting the ticket's queue.
        [[nodiscard]] SubmissionQueue* queueFor(SubmissionTicket ticket) noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] RenderResult<void> configureUploads(const TransferOptions&, execution::CpuScheduler) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadImage(
            const ImageDescription&, std::span<const std::byte>, std::span<const ImageTransferRegion>,
            std::shared_ptr<const Image> clone = {}, bool graphics_queue = false,
            SubmissionRequirements* clone_last_use = nullptr) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadBuffer(
            std::span<const std::byte>, VkBufferUsageFlags usage) noexcept;
        [[nodiscard]] TransferStats uploadStats() const noexcept;
        [[nodiscard]] ResourceRegistry& resources() noexcept { return resources_; }
        [[nodiscard]] const ResourceRegistry& resources() const noexcept { return resources_; }
        [[nodiscard]] StorageSet& globalStorages() noexcept { return globals_; }

    private:
        friend struct NativeMemoryAudit;
        friend class Transfer;
        struct UploadState;
        friend class ExecutionContext;
        DeviceSession(VulkanDevice device, VulkanAllocator allocator) noexcept;
        VulkanDevice device_;
        VulkanAllocator allocator_;
        std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
        std::array<SubmissionQueue*, 3> queues_{};
        // CPU Context owners; accepted batches independently retain their physical resource sets.
        // Member destruction is explicitly ordered in the destructor.
        std::vector<std::unique_ptr<ExecutionContext>> contexts_;
        std::unique_ptr<StagingArena> staging_arena_;
        std::unique_ptr<UploadState> uploads_;
        ResourceRegistry resources_;
        StorageSet globals_;
        std::weak_ptr<DescriptorArray> sampled_images_;
    };
}
