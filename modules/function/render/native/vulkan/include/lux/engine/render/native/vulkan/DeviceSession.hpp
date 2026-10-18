#pragma once
#include <lux/engine/core/execution/Scheduler.hpp>

#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/resource/Binding.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class DescriptorArray;
    class ExecutionContext;
    class Transfer;
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
            std::uint32_t slots = 3,
            std::size_t retirement_capacity = 0
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
        [[nodiscard]] RetirementQueue& retirement(EQueueRole role = EQueueRole::GRAPHICS) noexcept
        {
            return *retirements_[static_cast<std::size_t>(role)];
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
        [[nodiscard]] RenderResult<BufferPermit> createBuffer(
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access
        ) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> createImage(const ImageDescription& description) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> createImage(VkExtent2D extent, VkFormat format,
            VkImageUsageFlags usage, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> aliasImage(const ImagePermit& source) noexcept;
        // Synchronous cold borrow. No stale pointer is read during identity validation.
        [[nodiscard]] Buffer* borrow(const BufferPermit& permit) const noexcept;
        [[nodiscard]] Image* borrow(const ImagePermit& permit) const noexcept;
        // Caller proves no accepted GPU work uses this candidate. Failure preserves the permit.
        [[nodiscard]] RenderResult<void> destroyUnsubmitted(BufferPermit& permit) noexcept;
        [[nodiscard]] RenderResult<void> destroyUnsubmitted(ImagePermit& permit) noexcept;
        // Cold completed-use release; accepted GPU work never goes through destroyUnsubmitted.
        [[nodiscard]] RenderResult<void> releaseCompleted(BufferPermit& permit, SubmissionTicket last_use) noexcept;
        [[nodiscard]] RenderResult<void> releaseCompleted(ImagePermit& permit, SubmissionTicket last_use) noexcept;
        [[nodiscard]] RenderResult<void> releaseCompleted(BufferPermit& permit, const SubmissionRequirements&) noexcept;
        [[nodiscard]] RenderResult<void> releaseCompleted(ImagePermit& permit, const SubmissionRequirements&) noexcept;

    private:
        friend struct NativeMemoryAudit;
        friend class RetirementQueue;
        friend class Transfer;
        struct UploadState;
        [[nodiscard]] RenderResult<Buffer*> createBufferRecord(
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access,
            std::span<const std::uint32_t> families = {}) noexcept;
        [[nodiscard]] RenderResult<Image*> createImageRecord(const ImageDescription& description) noexcept;
        [[nodiscard]] bool owns(const Buffer* value) const noexcept;
        [[nodiscard]] bool owns(const Image* value) const noexcept;
        [[nodiscard]] bool owns(const SubmissionRequirements&) const noexcept;
        // Trusted internal borrows only: Context completion, retirement, or fully drained Session shutdown.
        void destroyFinished(Buffer* value) noexcept;
        void destroyFinished(Image* value) noexcept;
        friend class ExecutionContext;
        DeviceSession(VulkanDevice device, VulkanAllocator allocator) noexcept;
        VulkanDevice device_;
        VulkanAllocator allocator_;
        cxx::StableSlotMap<std::shared_ptr<Buffer>, BufferAllocationTag> buffers_;
        cxx::StableSlotMap<std::shared_ptr<Image>, ImageAllocationTag> images_;
        std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
        std::array<SubmissionQueue*, 3> queues_{};
        std::array<std::optional<RetirementQueue>, 3> retirement_owners_;
        std::array<RetirementQueue*, 3> retirements_{};
        // Unique physical owners. Context leases borrow entries until their real
        // GPU ticket retires. Member destruction is explicitly ordered in the destructor.
        std::vector<std::unique_ptr<ExecutionContext>> contexts_;
        std::unique_ptr<UploadState> uploads_;
        ResourceRegistry resources_;
        StorageSet globals_;
        std::weak_ptr<DescriptorArray> sampled_images_;
    };
}
