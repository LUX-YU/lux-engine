#include <lux/engine/render/frame/FrameStorage.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace lux::render
{
    FrameStorage::FrameStorage(std::size_t block_size) noexcept : block_size_(std::max<std::size_t>(block_size, 1)) {}

    FrameStorage::~FrameStorage() noexcept
    {
        clear();
    }

    FrameStorage::FrameStorage(FrameStorage&& other) noexcept : FrameStorage(other.block_size_)
    {
        *this = std::move(other);
    }

    FrameStorage& FrameStorage::operator=(FrameStorage&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            blocks_.swap(other.blocks_);
            used_blocks_.swap(other.used_blocks_);
            entries_.swap(other.entries_);
            phases_.swap(other.phases_);
            completion_observers_.swap(other.completion_observers_);
            block_size_ = other.block_size_;
            current_block_ = std::exchange(other.current_block_, 0);
        }
        return *this;
    }

    void FrameStorage::clear() noexcept
    {
        for (auto& block : blocks_)
        {
            ::operator delete(block.data, std::align_val_t{block.alignment});
        }
        blocks_.clear();
        reset();
    }

    void FrameStorage::reset() noexcept
    {
        used_blocks_.clear();
        entries_.clear();
        phases_.clear();
        completion_observers_.clear();
        current_block_ = 0;
    }

    void FrameStorage::deliverCompletion(const FrameCompletion& result) noexcept
    {
        // Detach first: nested completion delivery cannot invoke the same observers twice.
        // Restore the empty vector afterwards so repeated frames retain its allocation.
        std::vector<CompletionObserver> ready;
        ready.swap(completion_observers_);
        for (const auto& observer : ready)
        {
            observer.completed(observer.data, result);
        }
        ready.clear();
        ready.swap(completion_observers_);
    }

    RenderFrame FrameStorage::seal(std::uint64_t sequence, bool resynchronize) const noexcept
    {
        return {sequence, entries_, phases_, used_blocks_, resynchronize};
    }

    std::size_t FrameStorage::retainedBytes() const noexcept
    {
        std::size_t result = 0;
        for (const auto& block : blocks_)
        {
            result += block.capacity;
        }
        return result;
    }

    RenderResult<FrameStorage::Allocation>
    FrameStorage::allocate(std::size_t bytes, std::size_t alignment) noexcept
    {
        for (;; ++current_block_)
        {
            if (current_block_ >= UINT32_MAX)
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            if (current_block_ == blocks_.size())
            {
                const auto capacity = std::max(block_size_, bytes);
                const auto block_alignment = std::max(alignof(std::max_align_t), alignment);
                auto* data = static_cast<std::byte*>(::operator new(capacity, std::align_val_t{block_alignment}));
                blocks_.push_back({data, capacity, block_alignment});
            }
            if (used_blocks_.size() <= current_block_)
            {
                used_blocks_.emplace_back();
            }
            const auto& block = blocks_[current_block_];
            if (block.alignment < alignment)
            {
                continue;
            }
            const auto used = used_blocks_[current_block_].size();
            const auto padding = (alignment - used % alignment) % alignment;
            const bool fits_padding = padding <= block.capacity - used;
            const bool fits_data = fits_padding && bytes <= block.capacity - used - padding;
            if (!fits_data)
            {
                continue;
            }
            const auto offset = used + padding;
            used_blocks_[current_block_] = {block.data, offset + bytes};
            return Allocation{
                block.data + offset,
                {static_cast<std::uint32_t>(current_block_), offset, bytes}
            };
        }
    }

    RenderResult<void> FrameStorage::append(std::span<const FrameEntry> entries) noexcept
    {
        if (entries.empty())
        {
            return {};
        }
        const bool exceeds_entries = entries.size() > UINT32_MAX - entries_.size();
        if (exceeds_entries)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        phases_.push_back({static_cast<std::uint32_t>(entries_.size()), static_cast<std::uint32_t>(entries.size())});
        entries_.insert(entries_.end(), entries.begin(), entries.end());
        return {};
    }
} // namespace lux::render
