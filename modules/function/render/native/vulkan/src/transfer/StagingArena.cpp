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
        auto arena = std::unique_ptr<StagingArena>(new StagingArena(allocator.native()));
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
            auto* mapped = buffer->mappedBytes(allocator.native()).data();
            arena->pages_.push_back({std::move(*buffer), mapped});
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
        for (auto& page : pages_)
        {
            page.buffer.release(allocator_);
        }
    }

    RenderResult<StagingPage> StagingArena::acquire() noexcept
    {
        if (free_pages_.empty())
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        auto page = StagingPage(*this, free_pages_.back());
        free_pages_.pop_back();
        return page;
    }

    StagingPage::~StagingPage() noexcept
    {
        if (arena_)
        {
            (void)finishMeasurement(); // Diagnostics cannot change page reuse after actual Copy completion.
            arena_->free_pages_.push_back(index_);
        }
    }

    StagingPage::StagingPage(StagingPage&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), index_(other.index_),
          measurement_(std::exchange(other.measurement_, nullptr)), timestamp_bits_(other.timestamp_bits_)
    {
    }

    std::span<std::byte> StagingPage::bytes() const noexcept
    {
        return {static_cast<std::byte*>(arena_->pages_[index_].mapped), std::size_t(buffer().size())};
    }

    VkResult StagingPage::flush(VkDeviceSize bytes) const noexcept
    {
        return buffer().flush(arena_->allocator_, 0, bytes);
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

    gapi::vk::VmaBuffer& StagingPage::buffer() const noexcept
    {
        return arena_->pages_[index_].buffer;
    }
} // namespace lux::render::vulkan
