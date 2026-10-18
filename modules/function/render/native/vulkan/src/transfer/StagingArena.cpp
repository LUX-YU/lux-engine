#include "StagingArena.hpp"
#include <exception>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    RenderResult<std::unique_ptr<StagingArena>> StagingArena::create(
        VulkanAllocator& allocator,
        std::size_t page_bytes,
        std::size_t page_count,
        std::span<const std::uint32_t> families
    ) noexcept
    {
        const bool is_invalid_capacity = !page_bytes || !page_count;
        if (is_invalid_capacity)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto arena = std::unique_ptr<StagingArena>(new StagingArena);
        arena->pages_.reserve(page_count);
        arena->free_pages_.reserve(page_count);
        for (std::size_t i = 0; i < page_count; ++i)
        {
            auto buffer =
                allocator.createBuffer(page_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD, families);
            if (!buffer)
            {
                return cxx::unexpected(buffer.error());
            }
            arena->pages_.push_back(std::move(**buffer));
            arena->free_pages_.push_back(i);
        }
        return arena;
    }

    StagingArena::~StagingArena() noexcept
    {
        // Session must release accepted batches and joined CPU work before destroying their arena.
        if (free_pages_.size() != pages_.size())
        {
            std::terminate();
        }
    }

    RenderResult<std::unique_ptr<StagingPage>> StagingArena::acquire() noexcept
    {
        if (free_pages_.empty())
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        auto page = std::unique_ptr<StagingPage>(new StagingPage(*this, free_pages_.back()));
        free_pages_.pop_back();
        return page;
    }

    StagingPage::~StagingPage() noexcept
    {
        (void)finishMeasurement(); // Failure is retained in the UploadContext measurement counters.
        arena_.free_pages_.push_back(index_);
    }

    void StagingPage::markMeasurement(UploadContext& context, std::uint32_t bits) noexcept
    {
        measurement_ = &context;
        timestamp_bits_ = bits;
    }

    RenderResult<void> StagingPage::finishMeasurement() noexcept
    {
        auto* context = std::exchange(measurement_, nullptr);
        return context ? context->measure(index_, timestamp_bits_) : RenderResult<void>{};
    }

    Buffer& StagingPage::buffer() const noexcept
    {
        return arena_.pages_[index_];
    }
} // namespace lux::render::vulkan
