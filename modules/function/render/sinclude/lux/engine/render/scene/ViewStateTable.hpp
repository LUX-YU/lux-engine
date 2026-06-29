#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lux::render
{
    /// Sparse-id keyed table optimized for view handle lookups and dense iteration.
    template <typename T>
    class ViewStateTable
    {
    public:
        [[nodiscard]] bool empty() const noexcept { return dense_values_.empty(); }
        [[nodiscard]] size_t size() const noexcept { return dense_values_.size(); }

        void clear()
        {
            sparse_.clear();
            dense_ids_.clear();
            dense_values_.clear();
        }

        [[nodiscard]] bool contains(uint32_t view_id) const noexcept
        {
            return view_id < sparse_.size() && sparse_[view_id] != 0u;
        }

        [[nodiscard]] T *find(uint32_t view_id) noexcept
        {
            if (!contains(view_id))
                return nullptr;
            return &dense_values_[sparse_[view_id] - 1u];
        }

        [[nodiscard]] const T *find(uint32_t view_id) const noexcept
        {
            if (!contains(view_id))
                return nullptr;
            return &dense_values_[sparse_[view_id] - 1u];
        }

        void set(uint32_t view_id, const T &value)
        {
            upsert(view_id, value);
        }

        void set(uint32_t view_id, T &&value)
        {
            upsert(view_id, std::move(value));
        }

        [[nodiscard]] bool erase(uint32_t view_id)
        {
            if (!contains(view_id))
                return false;

            const uint32_t index = sparse_[view_id] - 1u;
            const uint32_t last = static_cast<uint32_t>(dense_values_.size() - 1u);
            if (index != last)
            {
                dense_values_[index] = std::move(dense_values_[last]);
                dense_ids_[index] = dense_ids_[last];
                sparse_[dense_ids_[index]] = index + 1u;
            }

            dense_values_.pop_back();
            dense_ids_.pop_back();
            sparse_[view_id] = 0u;
            return true;
        }

    private:
        template <typename U>
        void upsert(uint32_t view_id, U &&value)
        {
            if (contains(view_id))
            {
                dense_values_[sparse_[view_id] - 1u] = std::forward<U>(value);
                return;
            }

            if (view_id >= sparse_.size())
                sparse_.resize(static_cast<size_t>(view_id) + 1u, 0u);

            dense_ids_.push_back(view_id);
            dense_values_.push_back(std::forward<U>(value));
            sparse_[view_id] = static_cast<uint32_t>(dense_values_.size());
        }

        std::vector<uint32_t> sparse_{}; // view_id -> dense_index + 1, 0 means missing
        std::vector<uint32_t> dense_ids_{};
        std::vector<T> dense_values_{};
    };

} // namespace lux::render
