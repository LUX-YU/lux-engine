#pragma once

#include <lux/engine/render/device/Submission.hpp>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;

    // A range value, not an Arena controller. The owning upload window returns or submits it once.
    struct StagingRange final
    {
        std::size_t page = SIZE_MAX;
        std::uint64_t offset = 0;
        nri::Buffer* buffer = nullptr;
        std::span<std::byte> bytes;
    };

    class StagingArena final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<StagingArena>> create(
            DeviceSession& session,
            std::size_t page_bytes,
            std::size_t page_count,
            std::uint64_t alignment
        ) noexcept;

        ~StagingArena() noexcept;
        StagingArena(const StagingArena&) = delete;
        StagingArena& operator=(const StagingArena&) = delete;

        // Legacy's aligned bump allocation, independently reclaimed for each finite backing page.
        [[nodiscard]] RenderResult<StagingRange> acquire(
            std::size_t bytes,
            std::size_t unit = 4,
            bool reserve_frame_page = false
        ) noexcept;
        [[nodiscard]] bool canAcquire(std::size_t unit, bool reserve_frame_page = false) const noexcept;

        [[nodiscard]] RenderResult<void> flush(const StagingRange&) noexcept;
        void markSubmitted(StagingRange&, SubmissionQueue&) noexcept;
        void cancel(StagingRange&) noexcept;

    private:
        struct Page;

        struct Completion final
        {
            SubmissionCallback node;
            StagingArena* arena = nullptr;
            SubmissionQueue* queue = nullptr;
            std::size_t page = 0;
            std::uint64_t accepted = 0;
        };

        struct Page final
        {
            nri::Buffer* buffer = nullptr;
            std::byte* mapped = nullptr;
            std::uint64_t head = 0;
            std::size_t cpu_ranges = 0;
            std::array<Completion, 3> completions;
        };

        StagingArena(DeviceSession&, std::size_t bytes, std::size_t count, std::uint64_t alignment) noexcept;
        static void completed(void*, RenderResult<void>) noexcept;
        void release(std::size_t) noexcept;
        void reclaim(Page&) noexcept;
        void commit(std::size_t, SubmissionQueue&) noexcept;

        DeviceSession& session_;
        std::uint64_t page_bytes_;
        std::uint64_t alignment_;
        std::vector<Page> pages_;
    };
} // namespace lux::render::detail
