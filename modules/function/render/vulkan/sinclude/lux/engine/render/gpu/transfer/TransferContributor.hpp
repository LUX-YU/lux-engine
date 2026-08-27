#pragma once
/**
 * @file TransferContributor.hpp
 * @brief Non-virtual registration mechanism for upload contributors.
 *
 * Each resource class that performs CPU→GPU transfers implements a
 * `submitTransfers(TransferScheduler&)` method.  At registration time,
 * a type-erased function pointer is stored (no virtual dispatch).
 *
 * The scheduler iterates contributors in priority order and invokes
 * their submit functions.
 */

#include <cstdint>
#include <vector>
#include <algorithm>

namespace lux::render
{

    class TransferScheduler;

    // =========================================================================
    //  TransferContributorEntry — type-erased, non-virtual contributor slot
    // =========================================================================

    struct TransferContributorEntry
    {
        using SubmitFn = void (*)(void* resource, TransferScheduler& scheduler);
        using PostTransferFn = void (*)(void* resource, VkCommandBuffer cmd);

        void* resource{nullptr};
        SubmitFn submit_fn{nullptr};
        PostTransferFn post_transfer_fn{nullptr}; ///< Optional: called after endTransfers (e.g. mip gen)
        int priority{0};                          ///< Lower values execute first (e.g. grow=-1, data=0, scene=1)
    };

    // =========================================================================
    //  Type-safe registration helper
    // =========================================================================

    /**
     * @brief Create a TransferContributorEntry from a typed resource pointer.
     *
     * Requires T to have a `void submitTransfers(TransferScheduler&)` method.
     */
    template <typename T> TransferContributorEntry makeTransferContributor(T* resource, int priority = 0) noexcept
    {
        return TransferContributorEntry{
            .resource = static_cast<void*>(resource),
            .submit_fn = [](void* r, TransferScheduler& s) { static_cast<T*>(r)->submitTransfers(s); },
            .priority = priority,
        };
    }

    /**
     * @brief Create a TransferContributorEntry with a post-transfer callback.
     *
     * Requires T to have:
     *   - void submitTransfers(TransferScheduler&)
     *   - void postTransfer(VkCommandBuffer)
     *
     * The post-transfer callback is invoked after endTransfers() for operations
     * that depend on copy completion (e.g. runtime mip generation).
     */
    template <typename T>
    TransferContributorEntry makeTransferContributorWithPost(T* resource, int priority = 0) noexcept
    {
        return TransferContributorEntry{
            .resource = static_cast<void*>(resource),
            .submit_fn = [](void* r, TransferScheduler& s) { static_cast<T*>(r)->submitTransfers(s); },
            .post_transfer_fn = [](void* r, VkCommandBuffer cmd) { static_cast<T*>(r)->postTransfer(cmd); },
            .priority = priority,
        };
    }

    // =========================================================================
    //  TransferContributorList — sorted container used by registries
    // =========================================================================

    class TransferContributorList
    {
    public:
        void add(TransferContributorEntry entry)
        {
            entries_.push_back(entry);
            dirty_ = true;
        }

        void remove(void* resource) noexcept
        {
            entries_.erase(
                std::remove_if(
                    entries_.begin(),
                    entries_.end(),
                    [resource](const TransferContributorEntry& e) { return e.resource == resource; }
                ),
                entries_.end()
            );
        }

        void clear() noexcept
        {
            entries_.clear();
            dirty_ = false;
        }

        /// Execute all contributors in priority order.
        void dispatch(TransferScheduler& scheduler)
        {
            if (dirty_)
            {
                std::stable_sort(
                    entries_.begin(),
                    entries_.end(),
                    [](const TransferContributorEntry& a, const TransferContributorEntry& b) {
                        return a.priority < b.priority;
                    }
                );
                dirty_ = false;
            }
            for (auto& entry : entries_)
            {
                if (entry.submit_fn && entry.resource)
                    entry.submit_fn(entry.resource, scheduler);
            }
        }

        /// Execute post-transfer callbacks for contributors that have them.
        /// Called after endTransfers() for operations like mip generation.
        void dispatchPostTransfer(VkCommandBuffer cmd)
        {
            for (auto& entry : entries_)
            {
                if (entry.post_transfer_fn && entry.resource)
                    entry.post_transfer_fn(entry.resource, cmd);
            }
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return entries_.empty();
        }
        [[nodiscard]] size_t size() const noexcept
        {
            return entries_.size();
        }

    private:
        std::vector<TransferContributorEntry> entries_;
        bool dirty_{false};
    };

} // namespace lux::render
