#pragma once
/**
 * @file FrameRetireScheduler.hpp
 * @brief Serial-based deferred callback scheduler for GPU resource retirement.
 *
 * Callbacks are deferred until a given frame serial is guaranteed complete
 * (i.e. the GPU fence for that frame has been waited on).
 *
 * Thread safety: NOT thread-safe.  All calls must be on the render thread.
 */

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class FrameRetireScheduler
    {
    public:
        using Callback = std::function<void()>;
        using OwnerToken = uint64_t;

        /// Schedule a callback to run once `completed_serial >= retire_serial`.
        void defer(uint64_t retire_serial, OwnerToken owner_token, Callback cb)
        {
            uint32_t id = 0;
            if (!free_entries_.empty())
            {
                id = free_entries_.back();
                free_entries_.pop_back();
            }
            else
            {
                id = static_cast<uint32_t>(entries_.size());
                entries_.emplace_back();
            }

            auto& owner_entries = owner_index_[owner_token];
            Entry& entry = entries_[id];
            entry.retire_serial = retire_serial;
            entry.owner_token = owner_token;
            entry.callback = std::move(cb);
            entry.owner_pos = owner_entries.size();
            entry.active = true;

            owner_entries.push_back(id);
            retire_order_.push_back(id);
            ++pending_count_;
        }

        void purge(OwnerToken owner_token)
        {
            auto owner_it = owner_index_.find(owner_token);
            if (owner_it == owner_index_.end())
                return;

            auto ids = std::move(owner_it->second);
            owner_index_.erase(owner_it);

            for (uint32_t id : ids)
            {
                if (id >= entries_.size())
                    continue;
                Entry& entry = entries_[id];
                if (!entry.active)
                    continue;
                releaseEntry(id, /*owner_already_removed=*/true);
            }
        }

        /// Execute and remove all callbacks whose serial has been retired.
        void collect(uint64_t completed_serial)
        {
            // Drain the FIFO front-to-back: the oldest entry has the smallest
            // serial (frame-monotone), so once the front isn't complete neither
            // is anything behind it. Inactive (purged) fronts are popped + skipped.
            while (!retire_order_.empty())
            {
                const uint32_t id = retire_order_.front();
                if (id < entries_.size() && entries_[id].active && entries_[id].retire_serial > completed_serial)
                    break;

                retire_order_.pop_front();
                if (id >= entries_.size() || !entries_[id].active)
                    continue;

                Callback cb = std::move(entries_[id].callback);
                releaseEntry(id, /*owner_already_removed=*/false);
                if (cb)
                    cb();
            }
        }

        /// Execute all pending callbacks immediately (shutdown).
        void flushAll()
        {
            for (uint32_t id = 0; id < entries_.size(); ++id)
            {
                Entry& entry = entries_[id];
                if (!entry.active)
                    continue;

                Callback cb = std::move(entry.callback);
                releaseEntry(id, /*owner_already_removed=*/true);
                if (cb)
                    cb();
            }
            owner_index_.clear();
            retire_order_.clear();
        }

        [[nodiscard]] size_t pendingCount() const noexcept
        {
            return pending_count_;
        }

    private:
        struct Entry
        {
            uint64_t retire_serial{0};
            OwnerToken owner_token{0};
            Callback callback{};
            size_t owner_pos{0};
            bool active{false};
        };

        void releaseEntry(uint32_t id, bool owner_already_removed)
        {
            Entry& entry = entries_[id];
            if (!entry.active)
                return;

            if (!owner_already_removed)
                detachFromOwnerIndex(id);

            entry.active = false;
            entry.callback = nullptr;
            entry.retire_serial = 0;
            entry.owner_token = 0;
            entry.owner_pos = 0;
            free_entries_.push_back(id);
            --pending_count_;
        }

        void detachFromOwnerIndex(uint32_t id)
        {
            Entry& entry = entries_[id];
            auto owner_it = owner_index_.find(entry.owner_token);
            if (owner_it == owner_index_.end())
                return;

            auto& ids = owner_it->second;
            const size_t pos = entry.owner_pos;

            if (pos < ids.size() && ids[pos] == id)
            {
                const uint32_t moved = ids.back();
                ids[pos] = moved;
                ids.pop_back();
                if (moved != id)
                    entries_[moved].owner_pos = pos;
            }
            else
            {
                for (size_t i = 0; i < ids.size(); ++i)
                {
                    if (ids[i] != id)
                        continue;
                    const uint32_t moved = ids.back();
                    ids[i] = moved;
                    ids.pop_back();
                    if (moved != id)
                        entries_[moved].owner_pos = i;
                    break;
                }
            }

            if (ids.empty())
                owner_index_.erase(owner_it);
        }

        std::vector<Entry> entries_;
        std::vector<uint32_t> free_entries_;
        // FIFO of entry ids in non-decreasing retire_serial (frame-monotone).
        // Replaces a std::map<serial, vector<id>> used purely as a monotone
        // drain-from-front queue — the deque reuses its blocks (no per-new-serial
        // tree node / inner-vector allocation) and is O(1) push-back/pop-front.
        // owner_index_ stays a hash map: it serves random erase-by-owner (purge),
        // which has no ordering and is correctly an unordered_map. (P-5)
        std::deque<uint32_t> retire_order_;
        std::unordered_map<OwnerToken, std::vector<uint32_t>> owner_index_;
        size_t pending_count_{0};
    };

} // namespace lux::render
