#pragma once

#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class StagingPage;

    // One bounded CPU/staging/GPU transfer state machine, used by both Buffer and Image storage.
    // Source bytes are borrowed from the professional operation; actual destination and Clone Images are shared.
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

        [[nodiscard]] std::optional<SubmissionTicket> lastSubmission() const noexcept
        {
            return ticket_;
        }

        [[nodiscard]] const SubmissionRequirements& requirements() const noexcept
        {
            return requirements_;
        }

        [[nodiscard]] bool completed() const noexcept
        {
            return phase_ == EPhase::COMPLETE;
        }

        [[nodiscard]] RenderResult<void> result() const noexcept;

        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return session_ == &session && !started_ && !failure_;
        }

        [[nodiscard]] const Image& image() const noexcept;
        [[nodiscard]] const Buffer& buffer() const noexcept;

        [[nodiscard]] const std::shared_ptr<Image>& imageResource() const noexcept
        {
            return image_;
        }

        [[nodiscard]] RenderResult<std::shared_ptr<Buffer>> takeBuffer() noexcept;

        [[nodiscard]] std::uint64_t prepareNs() const noexcept
        {
            return prepare_ns_;
        }

    private:
        friend class DeviceSession;
        enum class EPhase
        {
            PREPARE,
            CPU_PENDING,
            COPY,
            COPY_PENDING,
            ACQUIRE,
            FINAL_PENDING,
            COMPLETE
        };
        Transfer(DeviceSession&, std::span<const std::byte>, std::shared_ptr<StagingPage>) noexcept;
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
        std::shared_ptr<Image> image_;
        std::shared_ptr<Buffer> buffer_;
        std::shared_ptr<const Image> clone_;
        SubmissionRequirements* clone_last_use_ = nullptr;
        std::vector<ImageTransferRegion> regions_;
        std::shared_ptr<StagingPage> staging_page_;
        std::size_t region_ = 0, offset_ = 0;
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
} // namespace lux::render::vulkan
