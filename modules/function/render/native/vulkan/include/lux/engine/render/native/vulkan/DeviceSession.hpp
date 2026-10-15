#pragma once

#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/features/Resources.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/features/Texture.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

namespace lux::render::vulkan
{
    class CompiledGraph;
    class ExecutionContext;
    class TextureUpload;
    class Transfer;
    struct ImageTransferRegion;
    class TextureStorage;
    class TextureVersion;
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
            std::size_t retirement_capacity = 64
        ) noexcept;
        ~DeviceSession() noexcept;
        DeviceSession(const DeviceSession&) = delete;
        DeviceSession& operator=(const DeviceSession&) = delete;

        [[nodiscard]] RenderResult<void> drain() noexcept;
        [[nodiscard]] RenderResult<void> collect() noexcept;
        [[nodiscard]] RenderResult<ExecutionContext*> prepare(
            std::shared_ptr<const CompiledGraph> code, const TargetDesc& target,
            const ResourceDomain& view
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

        [[nodiscard]] const VulkanDevice& device() const noexcept { return device_; }
        // Boundary lookup compares stable owner identity before inspecting the ticket's queue.
        [[nodiscard]] SubmissionQueue* queueFor(SubmissionTicket ticket) noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] RenderResult<void> configureUploads(const TransferOptions&) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<TextureUpload>> uploadTexture(
            const rdesc::TextureInfo&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadImage(
            const ImageDescription&, std::span<const std::byte>, std::span<const ImageTransferRegion>,
            const ImageAllocation* clone = nullptr, bool graphics_queue = false) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> uploadBuffer(
            std::span<const std::byte>, VkBufferUsageFlags usage) noexcept;
        [[nodiscard]] TransferStats uploadStats() const noexcept;
        void joinUploads() noexcept;
        [[nodiscard]] RenderResult<void> configureTextures(std::size_t capacity, std::uint64_t byte_limit) noexcept;
        [[nodiscard]] TextureStorage* textures() const noexcept;
        [[nodiscard]] ResourceRegistry& resources() noexcept { return *resources_; }
        [[nodiscard]] RenderResult<BufferPermit> createBuffer(
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access
        ) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> createImage(const ImageDescription& description) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> createImage(VkExtent2D extent, VkFormat format,
            VkImageUsageFlags usage, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) noexcept;
        [[nodiscard]] RenderResult<ImagePermit> aliasImage(const ImagePermit& source) noexcept;
        [[nodiscard]] RenderResult<void> write(BufferAllocation& buffer, VkDeviceSize offset,
            std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] RenderResult<void> read(const BufferAllocation& buffer, VkDeviceSize offset,
            std::span<std::byte> bytes) const noexcept;
        [[nodiscard]] BufferMemoryBinding memoryBinding(const BufferAllocation& buffer) const noexcept;
        [[nodiscard]] ImageMemoryBinding memoryBinding(const ImageAllocation& image) const noexcept;
        // Synchronous cold borrow. No stale pointer is read during identity validation.
        [[nodiscard]] BufferAllocation* borrow(const BufferPermit& permit) const noexcept;
        [[nodiscard]] ImageAllocation* borrow(const ImagePermit& permit) const noexcept;
        // Caller proves no accepted GPU work uses this candidate. Failure preserves the permit.
        [[nodiscard]] RenderResult<void> destroyUnsubmitted(BufferPermit& permit) noexcept;
        [[nodiscard]] RenderResult<void> destroyUnsubmitted(ImagePermit& permit) noexcept;
        // Cold completed-use release; accepted GPU work never goes through destroyUnsubmitted.
        [[nodiscard]] RenderResult<void> releaseCompleted(BufferPermit& permit, SubmissionTicket last_use) noexcept;
        [[nodiscard]] RenderResult<void> releaseCompleted(ImagePermit& permit, SubmissionTicket last_use) noexcept;

    private:
        friend struct NativeMemoryAudit;
        friend class RetirementQueue;
        friend class TextureUpload;
        friend class Transfer;
        friend class TextureStorage;
        struct UploadState;
        [[nodiscard]] RenderResult<std::unique_ptr<TextureUpload>> prepareTextureUpload(
            const rdesc::TextureInfo&, std::span<const std::byte>, std::optional<TextureRegion>) noexcept;
        [[nodiscard]] RenderResult<BufferAllocation*> createBufferRecord(
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access,
            std::span<const std::uint32_t> families = {}) noexcept;
        [[nodiscard]] RenderResult<ImageAllocation*> createImageRecord(const ImageDescription& description) noexcept;
        [[nodiscard]] bool owns(const BufferAllocation* value) const noexcept;
        [[nodiscard]] bool owns(const ImageAllocation* value) const noexcept;
        // Trusted internal borrows only: Context completion, retirement, or fully drained Session shutdown.
        void destroyFinished(BufferAllocation* value) noexcept;
        void destroyFinished(ImageAllocation* value) noexcept;
        friend class ExecutionContext;
        void refreshTexture(TextureId, TextureVersion&) noexcept;
        DeviceSession(VulkanDevice device, VulkanAllocator allocator) noexcept;
        VulkanDevice device_;
        VulkanAllocator allocator_;
        cxx::StableSlotMap<BufferAllocation, BufferAllocationTag> buffers_;
        cxx::StableSlotMap<ImageAllocation, ImageAllocationTag> images_;
        std::array<std::unique_ptr<SubmissionQueue>, 3> queue_owners_;
        std::array<SubmissionQueue*, 3> queues_{};
        std::array<std::optional<RetirementQueue>, 3> retirement_owners_;
        std::array<RetirementQueue*, 3> retirements_{};
        // Unique physical owners. Context leases borrow entries until their real
        // GPU ticket retires. Member destruction is explicitly ordered in the destructor.
        std::vector<std::unique_ptr<ExecutionContext>> contexts_;
        std::unique_ptr<UploadState> uploads_;
        std::unique_ptr<ResourceRegistry> resources_;
    };
}
