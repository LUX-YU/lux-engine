#pragma once

#include <functional>
#include <unordered_map>
#include <utility>

namespace lux::exec
{
    enum class CacheState
    {
        Pending,
        Ready,
        Failed,
    };

    /// Main-thread-owned key/value state table. Scheduling belongs to the
    /// domain owner; this type only stores its committed state.
    template <class K,
              class V,
              class Hash = std::hash<K>,
              class Eq = std::equal_to<K>>
    class MainThreadStateCache
    {
    public:
        [[nodiscard]] const V* tryGet(const K& key) const noexcept
        {
            const auto it = slots_.find(key);
            return it != slots_.end() && it->second.state == CacheState::Ready
                ? &it->second.value
                : nullptr;
        }

        /// A missing key has not started production and therefore has the same
        /// externally observable state as an explicitly pending entry.
        [[nodiscard]] CacheState state(const K& key) const noexcept
        {
            const auto it = slots_.find(key);
            return it == slots_.end() ? CacheState::Pending : it->second.state;
        }

        /// Returns true only for the first request for a key.
        bool markPending(const K& key)
        {
            return slots_.try_emplace(
                key,
                Slot{CacheState::Pending, V{}}
            ).second;
        }

        void setReady(const K& key, V value)
        {
            if (auto it = slots_.find(key); it != slots_.end())
            {
                it->second.value = std::move(value);
                it->second.state = CacheState::Ready;
            }
        }

        void setFailed(const K& key)
        {
            if (auto it = slots_.find(key); it != slots_.end())
                it->second.state = CacheState::Failed;
        }

        void invalidate(const K& key)
        {
            slots_.erase(key);
        }

        void clear() noexcept
        {
            slots_.clear();
        }

        template <class Fn>
        void forEachReady(Fn&& fn) const
        {
            for (const auto& [key, slot] : slots_)
            {
                if (slot.state == CacheState::Ready)
                    fn(key, slot.value);
            }
        }

    private:
        struct Slot
        {
            CacheState state;
            V value;
        };

        std::unordered_map<K, Slot, Hash, Eq> slots_;
    };
} // namespace lux::exec
