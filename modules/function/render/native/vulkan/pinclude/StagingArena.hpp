#pragma once

#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <vector>

namespace lux::render::vulkan
{
    class StagingArena;

    // A disjoint mapped range borrowed by CPU preparation until commit or cancellation.
    // Accepted ranges are reclaimed by their page's existing Queue completion nodes.
    class StagingRange final
    {
    public:
        ~StagingRange() noexcept;
        StagingRange(StagingRange&&) noexcept;
        StagingRange& operator=(StagingRange&&) = delete;
        StagingRange(const StagingRange&) = delete;
        StagingRange& operator=(const StagingRange&) = delete;

        [[nodiscard]] VkBuffer buffer() const noexcept;

        [[nodiscard]] VkDeviceSize offset() const noexcept
        {
            return offset_;
        }

        [[nodiscard]] std::span<std::byte> bytes() const noexcept;
        [[nodiscard]] VkResult flush() const noexcept;
        void commit(SubmissionQueue&) noexcept;

    private:
        friend class StagingArena;
        StagingRange(StagingArena&, std::size_t, VkDeviceSize, std::size_t) noexcept;
        StagingArena* arena_;
        std::size_t page_;
        VkDeviceSize offset_;
        std::size_t bytes_;
    };

    class StagingArena final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<StagingArena>> create(
            VulkanAllocator& allocator,
            std::size_t page_bytes,
            std::size_t page_count,
            std::span<const std::uint32_t> families,
            VkDeviceSize alignment
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

    private:
        friend class StagingRange;
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
            gapi::vk::VmaBuffer buffer;
            std::byte* mapped = nullptr;
            VkDeviceSize head = 0;
            std::size_t cpu_ranges = 0;
            std::array<Completion, 3> completions;
        };

        StagingArena(VmaAllocator allocator, std::size_t count, VkDeviceSize alignment) noexcept;
        static void completed(void*, RenderResult<void>) noexcept;
        void release(std::size_t) noexcept;
        void reclaim(Page&) noexcept;
        void commit(std::size_t, SubmissionQueue&) noexcept;

        VmaAllocator allocator_;
        VkDeviceSize alignment_;
        std::vector<Page> pages_;
    };
} // namespace lux::render::vulkan
