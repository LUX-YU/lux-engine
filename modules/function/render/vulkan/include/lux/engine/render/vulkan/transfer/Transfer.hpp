#pragma once

#include <vector>
#include <lux/engine/render/vulkan/memory/Memory.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

namespace lux::render::vulkan
{
    class StagingArena;

    // Borrow of immutable staged bytes, valid only within its originating batch.
    class StagingSlice
    {
    public:
        [[nodiscard]] VkBuffer native() const noexcept
        {
            return buffer_;
        }

        [[nodiscard]] VkDeviceSize offset() const noexcept
        {
            return offset_;
        }

        [[nodiscard]] VkDeviceSize size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] bool belongsTo(const CommandBatch &batch) const noexcept;

    private:
        friend class StagingArena;
        StagingSlice(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const CommandBatch &batch) noexcept;
        VkBuffer buffer_;
        VkDeviceSize offset_;
        VkDeviceSize size_;
        const SubmissionQueue *queue_;
        std::uint64_t recording_id_;
    };

    // Fixed partitions; a partition is reused only after its submission fence.
    // Borrows queue and allocator. No live CommandBatch may outlive this owner;
    // final destruction joins admitted GPU work before releasing the staging buffer.
    class StagingArena
    {
    public:
        [[nodiscard]] static RenderResult<StagingArena> create(
            SubmissionQueue &queue,
            const VulkanDevice &device,
            const VulkanAllocator &allocator,
            VkDeviceSize partition_bytes
        ) noexcept;
        ~StagingArena() noexcept;
        StagingArena(StagingArena &&other) noexcept;
        StagingArena &operator=(StagingArena &&) = delete;
        StagingArena(const StagingArena &) = delete;
        StagingArena &operator=(const StagingArena &) = delete;

        [[nodiscard]] RenderResult<StagingSlice> stage(
            const CommandBatch &batch, std::span<const std::byte> bytes, VkDeviceSize alignment = 4
        ) noexcept;

    private:
        struct Partition
        {
            VkDeviceSize head{};
            std::uint64_t recording_id{};
        };

        StagingArena(SubmissionQueue &queue, Buffer buffer, VkDeviceSize stride) noexcept;
        SubmissionQueue *queue_;
        Buffer buffer_;
        VkDeviceSize stride_;
        std::vector<Partition> partitions_;
    };

    // Single queue family, whole-resource conservative synchronization. Native
    // handles stay borrowed through completion; callers serialize all queue access.
    [[nodiscard]] RenderResult<void> recordUpload(
        const CommandBatch &batch, StagingSlice source, const Buffer &destination, VkDeviceSize offset = 0
    ) noexcept;
    [[nodiscard]] RenderResult<void> recordBufferCopy(
        const CommandBatch &batch,
        const Buffer &source,
        const Buffer &destination,
        VkDeviceSize size,
        VkDeviceSize source_offset = 0,
        VkDeviceSize destination_offset = 0,
        bool host_read = false
    ) noexcept;
    // Full one-layer RGBA8 image transfers. Layout is an explicit caller fact,
    // never a second resource-state authority. Both operations leave GENERAL.
    [[nodiscard]] RenderResult<void> recordImageUpload(
        const CommandBatch &batch, StagingSlice source, const Image &destination, VkImageLayout old_layout
    ) noexcept;
    [[nodiscard]] RenderResult<void> recordImageReadback(
        const CommandBatch &batch, const Image &source, VkImageLayout old_layout, const Buffer &destination
    ) noexcept;
} // namespace lux::render::vulkan
