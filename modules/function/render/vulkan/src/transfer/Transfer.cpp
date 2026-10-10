#include <lux/engine/render/vulkan/transfer/Transfer.hpp>

#include <exception>
#include <limits>
#include <utility>

namespace lux::render::vulkan
{
    namespace
    {
        constexpr auto kAll = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        constexpr auto kReadWrite = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        void bufferBarrier(
            VkCommandBuffer command,
            VkBuffer buffer,
            VkPipelineStageFlags2 source_stage,
            VkAccessFlags2 source_access,
            VkPipelineStageFlags2 destination_stage,
            VkAccessFlags2 destination_access
        ) noexcept
        {
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = source_stage;
            barrier.srcAccessMask = source_access;
            barrier.dstStageMask = destination_stage;
            barrier.dstAccessMask = destination_access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }

        void imageBarrier(
            VkCommandBuffer command,
            VkImage image,
            VkImageLayout old_layout,
            VkImageLayout new_layout,
            VkPipelineStageFlags2 source_stage,
            VkAccessFlags2 source_access,
            VkPipelineStageFlags2 destination_stage,
            VkAccessFlags2 destination_access
        ) noexcept
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = source_stage;
            barrier.srcAccessMask = source_access;
            barrier.dstStageMask = destination_stage;
            barrier.dstAccessMask = destination_access;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }

        bool fits(const Buffer &buffer, VkDeviceSize offset, VkDeviceSize size) noexcept
        {
            return offset <= buffer.size() && size <= buffer.size() - offset;
        }

        VkBufferImageCopy imageRegion(const Image &image, VkDeviceSize offset) noexcept
        {
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {image.extent().width, image.extent().height, 1};
            return region;
        }
    } // namespace

    StagingSlice::StagingSlice(
        VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const CommandBatch &batch
    ) noexcept
        : buffer_(buffer), offset_(offset), size_(size), queue_(&batch.owner()), recording_id_(batch.recordingId())
    {
    }

    bool StagingSlice::belongsTo(const CommandBatch &batch) const noexcept
    {
        return queue_ == &batch.owner() && recording_id_ == batch.recordingId();
    }

    StagingArena::StagingArena(SubmissionQueue &queue, Buffer buffer, VkDeviceSize stride) noexcept
        : queue_(&queue), buffer_(std::move(buffer)), stride_(stride), partitions_(queue.capacity())
    {
    }

    StagingArena::StagingArena(StagingArena &&other) noexcept
        : queue_(other.queue_), buffer_(std::move(other.buffer_)), stride_(other.stride_),
          partitions_(std::move(other.partitions_))
    {
    }

    StagingArena::~StagingArena() noexcept
    {
        if (!buffer_.native())
            return; // Moved ownership, not a public invalid state.
        queue_->completePending();
    }

    RenderResult<StagingArena> StagingArena::create(
        SubmissionQueue &queue,
        const VulkanDevice &device,
        const VulkanAllocator &allocator,
        VkDeviceSize partition_bytes
    ) noexcept
    {
        const bool is_wrong_owner = queue.device() != device.native() || allocator.device() != device.native();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        // Atom-isolated partitions keep non-coherent flushes away from GPU reads
        // in another in-flight partition. VMA handles allocation-relative rounding.
        const auto atom = device.properties().limits.nonCoherentAtomSize;
        const bool is_invalid_size = partition_bytes == 0 || partition_bytes % atom != 0 ||
                                     partition_bytes > std::numeric_limits<VkDeviceSize>::max() / queue.capacity();
        if (is_invalid_size)
            return cxx::unexpected(RenderError{kInvalidArgument});
        auto buffer = Buffer::create(
            allocator, partition_bytes * queue.capacity(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD
        );
        if (!buffer)
            return cxx::unexpected(buffer.error());
        return StagingArena{queue, std::move(*buffer), partition_bytes};
    }

    RenderResult<StagingSlice> StagingArena::stage(
        const CommandBatch &batch, std::span<const std::byte> bytes, VkDeviceSize alignment
    ) noexcept
    {
        if (&batch.owner() != queue_)
            return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_alignment = alignment == 0 || (alignment & (alignment - 1)) != 0;
        const bool is_invalid_input = bytes.empty() || is_invalid_alignment;
        if (is_invalid_input)
            return cxx::unexpected(RenderError{kInvalidArgument});
        auto &partition = partitions_[batch.slot()];
        if (partition.recording_id != batch.recordingId())
            partition = {0, batch.recordingId()};
        const auto base = stride_ * batch.slot();
        const auto current = base + partition.head;
        const auto padding = (alignment - current % alignment) % alignment;
        const bool is_full = padding > stride_ - partition.head || bytes.size() > stride_ - partition.head - padding;
        if (is_full)
            return cxx::unexpected(RenderError{kCapacity});
        const auto offset = current + padding;
        auto written = buffer_.write(offset, bytes);
        if (!written)
            return cxx::unexpected(written.error());
        partition.head += padding + bytes.size();
        return StagingSlice{buffer_.native(), offset, bytes.size(), batch};
    }

    RenderResult<void> recordUpload(
        const CommandBatch &batch, StagingSlice source, const Buffer &destination, VkDeviceSize offset
    ) noexcept
    {
        const bool is_wrong_owner = !source.belongsTo(batch) || destination.device() != batch.owner().device();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_copy = !fits(destination, offset, source.size()) || offset % 4 != 0 ||
                                     source.offset() % 4 != 0 || source.size() % 4 != 0 ||
                                     (destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0;
        if (is_invalid_copy)
            return cxx::unexpected(RenderError{kInvalidArgument});
        bufferBarrier(
            batch.native(),
            destination.native(),
            kAll,
            kReadWrite,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT
        );
        const VkBufferCopy copy{source.offset(), offset, source.size()};
        vkCmdCopyBuffer(batch.native(), source.native(), destination.native(), 1, &copy);
        bufferBarrier(
            batch.native(),
            destination.native(),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            kAll,
            kReadWrite
        );
        return {};
    }

    RenderResult<void> recordBufferCopy(
        const CommandBatch &batch,
        const Buffer &source,
        const Buffer &destination,
        VkDeviceSize size,
        VkDeviceSize source_offset,
        VkDeviceSize destination_offset,
        bool host_read
    ) noexcept
    {
        const bool is_wrong_owner =
            source.device() != batch.owner().device() || destination.device() != batch.owner().device();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_range = size == 0 || size % 4 != 0 || source_offset % 4 != 0 ||
                                      destination_offset % 4 != 0 || !fits(source, source_offset, size) ||
                                      !fits(destination, destination_offset, size);
        const bool is_invalid_usage = (source.usage() & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0 ||
                                      (destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0 ||
                                      source.native() == destination.native();
        const bool is_invalid_copy = is_invalid_range || is_invalid_usage;
        if (is_invalid_copy)
            return cxx::unexpected(RenderError{kInvalidArgument});
        bufferBarrier(
            batch.native(),
            source.native(),
            kAll,
            VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
        bufferBarrier(
            batch.native(),
            destination.native(),
            kAll,
            kReadWrite,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT
        );
        const VkBufferCopy copy{source_offset, destination_offset, size};
        vkCmdCopyBuffer(batch.native(), source.native(), destination.native(), 1, &copy);
        bufferBarrier(
            batch.native(),
            destination.native(),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            host_read ? VK_PIPELINE_STAGE_2_HOST_BIT : kAll,
            host_read ? VK_ACCESS_2_HOST_READ_BIT : kReadWrite
        );
        return {};
    }

    RenderResult<void> recordImageUpload(
        const CommandBatch &batch, StagingSlice source, const Image &destination, VkImageLayout old_layout
    ) noexcept
    {
        const bool is_wrong_owner = !source.belongsTo(batch) || destination.device() != batch.owner().device();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        if (destination.format() != VK_FORMAT_R8G8B8A8_UNORM)
            return cxx::unexpected(RenderError{kUnsupported});
        const auto size = VkDeviceSize{destination.extent().width} * destination.extent().height * 4;
        const bool is_invalid_copy = source.size() < size || source.offset() % 4 != 0 ||
                                     (destination.usage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0;
        if (is_invalid_copy)
            return cxx::unexpected(RenderError{kInvalidArgument});
        const bool is_discard = old_layout == VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier(
            batch.native(),
            destination.native(),
            old_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            is_discard ? VK_PIPELINE_STAGE_2_NONE : kAll,
            is_discard ? VK_ACCESS_2_NONE : kReadWrite,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT
        );
        const auto region = imageRegion(destination, source.offset());
        vkCmdCopyBufferToImage(
            batch.native(), source.native(), destination.native(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region
        );
        imageBarrier(
            batch.native(),
            destination.native(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            kAll,
            kReadWrite
        );
        return {};
    }

    RenderResult<void> recordImageReadback(
        const CommandBatch &batch, const Image &source, VkImageLayout old_layout, const Buffer &destination
    ) noexcept
    {
        const bool is_wrong_owner =
            source.device() != batch.owner().device() || destination.device() != batch.owner().device();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        if (source.format() != VK_FORMAT_R8G8B8A8_UNORM)
            return cxx::unexpected(RenderError{kUnsupported});
        const auto size = VkDeviceSize{source.extent().width} * source.extent().height * 4;
        const bool is_invalid_copy = destination.size() < size || old_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                                     (source.usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
                                     (destination.usage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0;
        if (is_invalid_copy)
            return cxx::unexpected(RenderError{kInvalidArgument});
        imageBarrier(
            batch.native(),
            source.native(),
            old_layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            kAll,
            kReadWrite,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
        bufferBarrier(
            batch.native(),
            destination.native(),
            kAll,
            kReadWrite,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT
        );
        const auto region = imageRegion(source, 0);
        vkCmdCopyImageToBuffer(
            batch.native(), source.native(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.native(), 1, &region
        );
        imageBarrier(
            batch.native(),
            source.native(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            kAll,
            kReadWrite
        );
        bufferBarrier(
            batch.native(),
            destination.native(),
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT
        );
        return {};
    }
} // namespace lux::render::vulkan
