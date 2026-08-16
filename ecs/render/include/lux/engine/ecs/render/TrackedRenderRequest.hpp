#pragma once
// ============================================================================
//  TrackedRenderRequest.hpp — ECS-safe ownership for one-shot render replies.
//
//  RenderRequest::then() may run synchronously for an already-settled request.
//  This table owns the request before installing its continuation; that
//  continuation only appends a value completion. Registry mutation and render
//  commands remain in the system's next update() safe point.
//
//  The table is owner-thread confined. RenderFrameSession reply pumping and ECS
//  schedule updates use that same thread, so no lock is needed. Abandoning an
//  active generation immediately frees its logical key: a re-entered entity may
//  issue a replacement while the old generation remains observable for cleanup.
// ============================================================================

#include <lux/engine/function/render/client/RenderRequest.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class ETrackedRequestStart
    {
        STARTED,
        DUPLICATE_KEY,
        INVALID_REQUEST
    };

    template <class Key, class Reply, class Context, class Hash = std::hash<Key>>
    class TrackedRenderRequest final
    {
        struct Pending
        {
            Pending(
                Key key,
                std::uint64_t serial,
                Context&& context,
                lux::render::RenderRequest<Reply>&& request
            )
                : key(std::move(key)),
                  serial(serial),
                  context(std::move(context)),
                  request(std::move(request))
            {
            }

            Key                                     key;
            std::uint64_t                           serial{0};
            Context                                 context;
            lux::render::ScopedRenderRequest<Reply> request;
            bool                                    abandoned{false};
        };

        struct Ready
        {
            std::uint64_t serial{0};
            Reply         reply{};
        };

    public:
        struct Completion
        {
            Key                      key;
            Context                  context;
            Reply                    reply{};
            lux::render::RenderError error{};
            bool                     abandoned{false};
            bool                     dispatch_failed{false};
        };

        TrackedRenderRequest() = default;
        ~TrackedRenderRequest() { clear(); }

        TrackedRenderRequest(const TrackedRenderRequest&)            = delete;
        TrackedRenderRequest& operator=(const TrackedRenderRequest&) = delete;
        TrackedRenderRequest(TrackedRenderRequest&&)                 = delete;
        TrackedRenderRequest& operator=(TrackedRenderRequest&&)      = delete;

        /// The issue function runs only after the active-key check. Accepting an
        /// already-issued create request would make DUPLICATE_KEY unable to reap
        /// the second request's eventual handle.
        template <class Issue>
        [[nodiscard]] ETrackedRequestStart start(
            const Key& key,
            Context context,
            Issue&& issue
        )
        {
            using Issued = std::remove_cvref_t<std::invoke_result_t<Issue&>>;
            static_assert(
                std::is_same_v<Issued, lux::render::RenderRequest<Reply>>,
                "issue must return RenderRequest<Reply>"
            );

            if (active_.contains(key))
                return ETrackedRequestStart::DUPLICATE_KEY;

            auto request = std::invoke(issue);
            if (!request.valid())
                return ETrackedRequestStart::INVALID_REQUEST;

            const std::uint64_t serial = nextSerial();
            auto [it, inserted] = pending_.try_emplace(
                serial,
                key,
                serial,
                std::move(context),
                std::move(request)
            );
            if (!inserted)   // only possible after a complete uint64 wrap
                return ETrackedRequestStart::INVALID_REQUEST;
            active_.emplace(key, serial);

            // Keep allocation out of an immediate continuation. OOM is process
            // fatal here, but callback work should still be deterministic.
            const std::size_t required = ready_.size() + pending_.size();
            if (ready_.capacity() < required)
            {
                const std::size_t capacity = ready_.capacity();
                const std::size_t grown = capacity == 0
                    ? 4
                    : capacity + capacity / 2;
                ready_.reserve(grown < required ? required : grown);
            }

            it->second.request.then(
                [this, serial](const Reply& reply)
                {
                    ready_.push_back(Ready{serial, reply});
                }
            );
            return ETrackedRequestStart::STARTED;
        }

        /// Whether @p key already has a current generation. Abandoned older
        /// generations deliberately do not block a replacement.
        [[nodiscard]] bool contains(const Key& key) const
        {
            return active_.contains(key);
        }

        [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }
        [[nodiscard]] bool hasCompletions() const noexcept { return !ready_.empty(); }

        /// Keep observing the old generation for cleanup, but detach it from its
        /// logical owner so the same key may immediately start a new generation.
        [[nodiscard]] bool abandon(const Key& key)
        {
            const auto active = active_.find(key);
            if (active == active_.end())
                return false;
            if (const auto pending = pending_.find(active->second);
                pending != pending_.end())
            {
                pending->second.abandoned = true;
            }
            active_.erase(active);
            return true;
        }

        template <class Predicate>
        std::size_t abandonIf(Predicate&& predicate)
        {
            std::size_t count = 0;
            auto&& matches = predicate;
            for (auto it = active_.begin(); it != active_.end();)
            {
                if (!matches(it->first))
                {
                    ++it;
                    continue;
                }
                if (const auto pending = pending_.find(it->second);
                    pending != pending_.end())
                {
                    pending->second.abandoned = true;
                }
                it = active_.erase(it);
                ++count;
            }
            return count;
        }

        /// Only use cancel when a reply cannot create newly-owned remote state.
        [[nodiscard]] bool cancel(const Key& key)
        {
            const auto active = active_.find(key);
            if (active == active_.end())
                return false;
            pending_.erase(active->second);
            active_.erase(active);
            return true;
        }

        /// Cancel all generations while returning their intent for local
        /// compensation (for example, re-dirtying a CPU export ledger).
        template <class Fn>
        void cancelAll(Fn&& fn)
        {
            auto pending = std::move(pending_);
            pending_.clear();
            active_.clear();
            ready_.clear();
            auto&& handler = fn;
            for (auto& [serial, value] : pending)
                handler(
                    value.key,
                    std::move(value.context),
                    value.abandoned
                );
        }

        /// Transfer all generations to a longer-lived owner. The receiver must
        /// synchronously install its continuation; that replaces the callback
        /// into this table before local completion storage is destroyed.
        template <class Fn>
        void handoffAll(Fn&& fn)
        {
            auto pending = std::move(pending_);
            pending_.clear();
            active_.clear();
            ready_.clear();
            auto&& handler = fn;
            for (auto& [serial, value] : pending)
                handler(
                    value.key,
                    std::move(value.context),
                    std::move(value.request),
                    value.abandoned
                );
        }

        void clear() noexcept
        {
            // Detach callbacks that capture this before destroying ready_.
            pending_.clear();
            active_.clear();
            ready_.clear();
        }

        template <class Fn>
        void drain(Fn&& fn)
        {
            // Completions produced by fn belong to the next drain, preventing a
            // synchronously-settled replacement from self-feeding this pass.
            auto ready = std::move(ready_);
            ready_.clear();
            auto&& handler = fn;

            for (auto& value : ready)
            {
                auto it = pending_.find(value.serial);
                if (it == pending_.end())
                    continue;   // cancelled generation; stale queued value

                Completion completion{
                    it->second.key,
                    std::move(it->second.context),
                    std::move(value.reply),
                    {},
                    it->second.abandoned,
                    it->second.request.failed()
                };
                if (completion.dispatch_failed)
                    completion.error = it->second.request.error();

                if (const auto active = active_.find(it->second.key);
                    active != active_.end() && active->second == value.serial)
                {
                    active_.erase(active);
                }
                // Callback already returned (or was immediate), so its lexical
                // owner can now be destroyed safely.
                pending_.erase(it);
                handler(std::move(completion));
            }
        }

    private:
        [[nodiscard]] std::uint64_t nextSerial() noexcept
        {
            const std::uint64_t result = next_serial_++;
            if (next_serial_ == 0)
                ++next_serial_;   // zero is the never-issued value
            return result;
        }

        std::unordered_map<std::uint64_t, Pending>   pending_;
        std::unordered_map<Key, std::uint64_t, Hash> active_;
        std::vector<Ready>                            ready_;
        std::uint64_t                                 next_serial_{1};
    };

} // namespace lux::ecs
