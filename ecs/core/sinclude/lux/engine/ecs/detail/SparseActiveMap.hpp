#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs::detail
{
    /// Sparse membership with a contiguous, allocation-free enumeration view.
    ///
    /// Mutation is O(1) on average. Erase uses swap-and-pop, so key order is
    /// intentionally unspecified and spans are invalidated by any mutation.
    /// This is a build-only implementation detail shared by Pixel and Tilemap.
    template <typename Key, typename Hash, typename Equal = std::equal_to<Key>>
    class SparseActiveMap final
    {
    public:
        static_assert(std::is_nothrow_copy_constructible_v<Key>);
        static_assert(std::is_nothrow_move_assignable_v<Key>);
        static_assert(std::is_nothrow_invocable_v<Hash, const Key&>);
        static_assert(std::is_nothrow_invocable_v<Equal, const Key&, const Key&>);

        [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

        [[nodiscard]] bool contains(const Key& key) const noexcept
        {
            const auto found = indices_.find(key);
            return found != indices_.end() && found->second != kInactive;
        }

        [[nodiscard]] std::span<const Key> keys() const noexcept
        {
            return keys_;
        }

        /// Registers a resident key. All storage needed by a later activate()
        /// is reserved here, keeping residency-state changes allocation-free.
        bool track(const Key& key, bool active) noexcept
        {
            const auto required = indices_.size() + 1u;
            if (keys_.capacity() < required)
            {
                const auto doubled = keys_.capacity() <=
                        std::numeric_limits<std::size_t>::max() / 2u
                    ? keys_.capacity() * 2u
                    : std::numeric_limits<std::size_t>::max();
                keys_.reserve(std::max(required, std::max<std::size_t>(
                    doubled,
                    8u)));
            }
            const auto [iterator, inserted] = indices_.try_emplace(
                key,
                kInactive);
            if (!inserted)
                return false;
            if (active)
            {
                keys_.push_back(key);
                iterator->second = keys_.size() - 1u;
            }
            return true;
        }

        bool activate(const Key& key) noexcept
        {
            const auto found = indices_.find(key);
            if (found == indices_.end() || found->second != kInactive)
                return false;
            // track() reserves one dense slot per resident key, so this push
            // cannot allocate. Key itself is constrained to no-throw copy.
            keys_.push_back(key);
            found->second = keys_.size() - 1u;
            return true;
        }

        bool deactivate(const Key& key) noexcept
        {
            const auto found = indices_.find(key);
            if (found == indices_.end() || found->second == kInactive)
                return false;

            const auto index = found->second;
            const auto last = keys_.size() - 1u;
            if (index != last)
            {
                const auto moved = indices_.find(keys_[last]);
                if (moved == indices_.end())
                    std::terminate();
                keys_[index] = std::move(keys_[last]);
                moved->second = index;
            }
            keys_.pop_back();
            found->second = kInactive;
            return true;
        }

        bool untrack(const Key& key) noexcept
        {
            const auto found = indices_.find(key);
            if (found == indices_.end())
                return false;
            if (found->second != kInactive && !deactivate(key))
                std::terminate();
            indices_.erase(found);
            return true;
        }

        void clear() noexcept
        {
            keys_.clear();
            indices_.clear();
        }

    private:
        static constexpr std::size_t kInactive =
            std::numeric_limits<std::size_t>::max();

        std::vector<Key> keys_;
        std::unordered_map<Key, std::size_t, Hash, Equal> indices_;
    };
} // namespace lux::ecs::detail
