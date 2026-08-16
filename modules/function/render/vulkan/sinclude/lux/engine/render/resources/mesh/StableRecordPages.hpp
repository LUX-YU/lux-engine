#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lux::render
{
    /// Append-only record storage whose existing element addresses never move.
    /// PageSize is part of the owning resource contract, not a growth heuristic.
    template<typename T, std::size_t PageSize>
    class StableRecordPages final
    {
    public:
        static_assert(PageSize != 0u);

        void reserve(std::size_t count)
        {
            pages_.reserve((count + PageSize - 1u) / PageSize);
        }

        void clear() noexcept
        {
            pages_.clear();
            size_ = 0u;
        }

        void push_back(T value)
        {
            if (size_ % PageSize == 0u)
                pages_.push_back(std::make_unique<T[]>(PageSize));
            (*this)[size_++] = std::move(value);
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] std::size_t pageCount() const noexcept
        {
            return pages_.size();
        }
        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return pages_.size() * PageSize;
        }

        [[nodiscard]] T& operator[](std::size_t index) noexcept
        {
            return pages_[index / PageSize][index % PageSize];
        }

        [[nodiscard]] const T& operator[](std::size_t index) const noexcept
        {
            return pages_[index / PageSize][index % PageSize];
        }

    private:
        std::vector<std::unique_ptr<T[]>> pages_{};
        std::size_t size_{0u};
    };
}
