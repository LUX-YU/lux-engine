#pragma once

#include <lux/engine/description/TextureInfo.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <atomic>
#include <lux/engine/render/resource/TransferOptions.hpp>
#include <lux/engine/render/features/Texture.hpp>

namespace lux::render::vulkan
{
    class TextureStorage;
    class TextureVersion;
    struct TextureUploadStats final
    {
        std::uint64_t copied_bytes = 0, chunks = 0, prepare_ns = 0, owner_ns = 0;
        std::uint64_t staging_bytes = 0, pending_bytes = 0, pending_peak = 0;
        std::uint64_t gpu_ns = 0, gpu_samples = 0;
    };

    // One cold Native candidate, never a Request owner or an independent completion registry.
    // Source is borrowed from the caller's retained original message until this object is destroyed.
    // advance is nonblocking; destroy only after completion/cancellation or host join + cold Session drain.
    class TextureUpload final
    {
    public:
        ~TextureUpload() noexcept;
        TextureUpload(const TextureUpload&) = delete;
        TextureUpload& operator=(const TextureUpload&) = delete;
        [[nodiscard]] RenderResult<bool> advance() noexcept;
        void cancel() noexcept { cancelled_ = true; }
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] const rdesc::TextureInfo& info() const noexcept { return info_; }
        [[nodiscard]] std::optional<SubmissionTicket> lastUse() const noexcept { return ticket_; }
        [[nodiscard]] const ImageAllocation& image() const noexcept;
        [[nodiscard]] RenderResult<ImagePermit> takeImage() noexcept;
        [[nodiscard]] RenderResult<void> publish() noexcept;
        [[nodiscard]] std::uint64_t prepareNs() const noexcept { return prepare_ns_; }
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return session_ == &session && phase_ == EPhase::PREPARE && !ticket_ && !cancelled_;
        }

    private:
        friend class DeviceSession;
        friend class TextureStorage;
        enum class EPhase { PREPARE, CPU_PENDING, COPY, COPY_PENDING, ACQUIRE, FINAL_PENDING, COMPLETE };
        TextureUpload(DeviceSession&, rdesc::TextureInfo, std::span<const std::byte>,
            ImagePermit, std::size_t, std::uint32_t, std::uint32_t) noexcept;
        static void prepare(void*) noexcept;
        DeviceSession* session_;
        rdesc::TextureInfo info_;
        std::span<const std::byte> source_;
        std::byte* mapped_;
        ImagePermit image_;
        std::size_t page_;
        std::uint32_t block_extent_, block_bytes_, mip_ = 0, row_ = 0, rows_ = 0;
        std::size_t row_bytes_ = 0, chunk_size_ = 0;
        std::atomic<bool> cpu_ready_{false};
        std::uint64_t prepare_ns_ = 0;
        std::optional<SubmissionTicket> ticket_;
        EPhase phase_ = EPhase::PREPARE;
        bool cancelled_ = false, initialized_ = false;
        TextureStorage* storage_ = nullptr;
        TextureVersion* version_ = nullptr;
        TextureId texture_;
        TextureVersion* base_ = nullptr;
        std::optional<TextureRegion> region_;
        SubmissionQueue* copy_queue_ = nullptr;
        bool query_pending_ = false;
    };
}
