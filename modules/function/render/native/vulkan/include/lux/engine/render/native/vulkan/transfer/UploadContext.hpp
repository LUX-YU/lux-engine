#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <exec/task.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class StagingPage;

    struct TransferStats final
    {
        std::uint64_t copied_bytes = 0, chunks = 0, prepare_ns = 0, owner_ns = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0, gpu_query_errors = 0;
    };

    struct ImageTransferRegion final
    {
        std::uint64_t source_offset = 0, source_row_pitch = 0;
        std::uint32_t block_extent = 1, block_bytes = 4;
        VkImageSubresourceLayers subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        VkOffset3D offset{};
        VkExtent3D extent{};
    };

    // One device's upload facilities. Tasks own their source and real target references; this object
    // owns neither professional candidates nor running tasks. Start with the RenderThread scheduler.
    class UploadContext final
    {
    public:
        using Result = RenderResult<SubmissionRequirements>;
        using Task = exec::task<Result>;

        ~UploadContext() noexcept;
        UploadContext(const UploadContext&) = delete;
        UploadContext& operator=(const UploadContext&) = delete;

        // Success means all required Copy/Acquire submissions were accepted. Their batches retain
        // resources through actual completion; success does not wait for the final fence observation.
        [[nodiscard]] Task writeBuffer(std::shared_ptr<Buffer> target, RenderPayload source) noexcept;
        [[nodiscard]] Task writeImage(
            std::shared_ptr<Image> target, RenderPayload source, std::vector<ImageTransferRegion> regions,
            std::shared_ptr<const Image> clone = {}, SubmissionRequirements* clone_use = nullptr
        ) noexcept;

        [[nodiscard]] TransferStats statistics() const noexcept { return stats; }

    private:
        friend class DeviceSession;
        friend class StagingPage;
        friend class Transfer;
        UploadContext(DeviceSession&, TransferOptions, execution::CpuScheduler) noexcept;
        [[nodiscard]] Task write(
            std::shared_ptr<Buffer>, std::shared_ptr<Image>, RenderPayload,
            std::vector<ImageTransferRegion>, std::shared_ptr<const Image>, SubmissionRequirements*
        ) noexcept;
        [[nodiscard]] RenderResult<void> measure(std::size_t page, std::uint32_t bits) noexcept;

        DeviceSession& session;
        SubmissionQueue* copy_queue = nullptr;
        TransferOptions options;
        execution::CpuScheduler cpu;
        TransferStats stats;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        std::uint32_t copy_timestamp_bits = 0, graphics_timestamp_bits = 0;
    };
}
