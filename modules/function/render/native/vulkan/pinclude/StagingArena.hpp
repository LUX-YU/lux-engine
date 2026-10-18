#pragma once

#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <vector>

namespace lux::render::vulkan
{
    class StagingArena;

    // One acquired mapped page. CPU preparation and accepted GPU batches share this same use.
    // The arena outlives all acquisitions; its free-list is accessed only on the RenderThread.
    class StagingPage final
    {
    public:
        ~StagingPage() noexcept;
        StagingPage(const StagingPage&) = delete;
        StagingPage& operator=(const StagingPage&) = delete;
        [[nodiscard]] Buffer& buffer() const noexcept;
        [[nodiscard]] std::size_t index() const noexcept { return index_; }

    private:
        friend class StagingArena;
        StagingPage(StagingArena& arena, std::size_t index) noexcept : arena_(arena), index_(index) {}
        StagingArena& arena_;
        std::size_t index_;
    };

    class StagingArena final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<StagingArena>> create(
            VulkanAllocator& allocator,
            std::size_t page_bytes,
            std::size_t page_count,
            std::span<const std::uint32_t> families
        ) noexcept;

        ~StagingArena() noexcept;
        StagingArena(const StagingArena&) = delete;
        StagingArena& operator=(const StagingArena&) = delete;
        [[nodiscard]] RenderResult<std::shared_ptr<StagingPage>> acquire() noexcept;
        [[nodiscard]] std::size_t available() const noexcept { return free_pages_.size(); }

    private:
        friend class StagingPage;
        StagingArena() noexcept = default;
        std::vector<std::shared_ptr<Buffer>> pages_;
        std::vector<std::size_t> free_pages_;
    };
}
