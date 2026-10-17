#pragma once

#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>

namespace lux::render::vulkan
{
    struct TransferStats final
    {
        std::uint64_t copied_bytes = 0, chunks = 0, prepare_ns = 0, owner_ns = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0;
    };

    // Native row-copy description. Texture format/color/revision validation belongs to its professional Storage.
    struct ImageTransferRegion final
    {
        std::uint64_t source_offset = 0, source_row_pitch = 0;
        std::uint32_t block_extent = 1, block_bytes = 4;
        VkImageSubresourceLayers subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        VkOffset3D offset{};
        VkExtent3D extent{};
    };

    // One bounded CPU/staging/GPU transfer state machine, used by both Buffer and Image storage.
    // Source and optional clone are borrowed; the professional work owner keeps them alive through completion.
    class Transfer final
    {
    public:
        ~Transfer() noexcept;
        Transfer(const Transfer&) = delete;
        Transfer& operator=(const Transfer&) = delete;
        using TerminalFn = void (*)(void*, RenderResult<void>) noexcept;
        [[nodiscard]] RenderResult<void> prepareCompletion() noexcept;
        void start(CompletionPost post, void* owner, TerminalFn terminal) noexcept;
        void cancel() noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] std::optional<SubmissionTicket> lastSubmission() const noexcept { return ticket_; }
        [[nodiscard]] const SubmissionRequirements& requirements() const noexcept { return requirements_; }
        [[nodiscard]] bool completed() const noexcept { return phase_ == EPhase::COMPLETE; }
        [[nodiscard]] RenderResult<void> result() const noexcept;
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return session_ == &session && !started_ && !failure_;
        }
        [[nodiscard]] const ImageAllocation& image() const noexcept;
        [[nodiscard]] const BufferAllocation& buffer() const noexcept;
        [[nodiscard]] RenderResult<ImagePermit> takeImage() noexcept;
        [[nodiscard]] RenderResult<BufferPermit> takeBuffer() noexcept;
        [[nodiscard]] std::uint64_t prepareNs() const noexcept { return prepare_ns_; }

    private:
        friend class DeviceSession;
        enum class EPhase { PREPARE, CPU_PENDING, COPY, COPY_PENDING, ACQUIRE, FINAL_PENDING, COMPLETE };
        Transfer(DeviceSession&, std::span<const std::byte>, std::size_t) noexcept;
        struct CpuPreparation;
        static void prepare(void*) noexcept;
        static void resumeCpu(void*, RenderResult<void>) noexcept;
        static void resumeGpu(void*, RenderResult<void>) noexcept;
        void continueTransfer() noexcept;
        void finish() noexcept;
        void recordImage(VkCommandBuffer command, bool acquiring, bool final) noexcept;
        void recordBuffer(VkCommandBuffer command, bool acquiring, bool final) noexcept;
        DeviceSession* session_;
        std::span<const std::byte> source_;
        std::byte* mapped_;
        ImagePermit image_;
        BufferPermit buffer_;
        const ImageAllocation* clone_ = nullptr;
        SubmissionRequirements* clone_last_use_ = nullptr;
        std::vector<ImageTransferRegion> regions_;
        std::size_t page_, region_ = 0, offset_ = 0;
        std::uint32_t row_ = 0, rows_ = 0;
        std::size_t row_bytes_ = 0, chunk_size_ = 0;
        CompletionPost completion_post_;
        std::unique_ptr<CpuPreparation> cpu_preparation_;
        std::optional<RenderError> failure_;
        std::uint64_t prepare_ns_ = 0;
        std::optional<SubmissionTicket> ticket_;
        SubmissionRequirements requirements_;
        SubmissionCallback copy_callback_, graphics_callback_;
        void* terminal_owner_ = nullptr;
        TerminalFn terminal_ = nullptr;
        EPhase phase_ = EPhase::PREPARE;
        bool started_ = false, initialized_ = false, query_pending_ = false;
        SubmissionQueue* copy_queue_ = nullptr;
    };
}
