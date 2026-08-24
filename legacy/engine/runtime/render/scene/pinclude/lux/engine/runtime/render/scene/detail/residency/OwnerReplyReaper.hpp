#pragma once
/**
 * @file OwnerReplyReaper.hpp
 * @brief Main-thread-confined lifecycle owner for one RenderRequest reply type.
 *
 * Owner-creating render RPCs cannot be cancelled once recorded.  This helper
 * owns their client continuations from wire submission through the MainThreadMailbox
 * adoption callback and exposes the exact pending count needed by async close:
 *
 *   pending = replies still on the wire + callbacks already posted to main.
 *
 * abandon() detaches the first set (destroying every owner already captured by
 * the domain handler) and turns the second set into compensation-only calls,
 * so a non-null owner carried by an already-arrived reply is still adopted and
 * released.  It is a control-plane primitive: all methods are main-thread
 * confined and it adds no lock, RTTI or exception channel.
 */

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace lux::runtime::detail
{
    template <class Reply>
    class OwnerReplyReaper final
    {
    public:
        explicit OwnerReplyReaper(TryPostToMain post_main) noexcept
            : post_main_(std::move(post_main))
            , control_(std::make_shared<Control>())
        {
            if (!post_main_)
                lux::render::renderFatal(
                    "Owner reply reaper requires a main-thread poster"
                );
        }

        ~OwnerReplyReaper() noexcept
        {
            abandon();
        }

        OwnerReplyReaper(const OwnerReplyReaper&) = delete;
        OwnerReplyReaper& operator=(const OwnerReplyReaper&) = delete;

        template <class Handler>
        void track(lux::render::RenderRequest<Reply> request, Handler handler)
        {
            static_assert(std::is_trivially_copyable_v<Reply>,
                          "owner reply must be wire-POD: destruction after "
                          "pending zero must have no owner semantics");
            static_assert(std::is_nothrow_copy_constructible_v<Reply>,
                          "owner reply must cross MainThreadScheduler by noexcept value copy");
            static_assert(std::is_nothrow_invocable_v<
                              Handler&, const Reply&, bool>,
                          "owner reply handler must be noexcept");

            if (control_->abandoning)
                lux::render::renderFatal(
                    "Owner reply registered after its reaper was abandoned"
                );
            if (!request.valid())
                lux::render::renderFatal(
                    "Owner reply reaper received a stateless request"
                );
            if (control_->next_token == 0)
                lux::render::renderFatal(
                    "Owner reply reaper token space exhausted"
                );

            auto owner = control_;
            const std::uint64_t token = owner->next_token++;
            auto [it, inserted] = owner->requests.emplace(
                token,
                lux::render::ScopedRenderRequest<Reply>{std::move(request)}
            );
            if (!inserted)
                lux::render::renderFatal(
                    "Owner reply reaper token collision"
                );

            // Strong Control capture is deliberate: once a reply transfers a
            // raw owner, losing the control block is not a legal drop path.
            // The finite cycle is broken by reply erase or abandon()/destruction.
            auto post = post_main_;
            it->second.then(
                [owner,
                 token,
                 post = std::move(post),
                 handler = std::move(handler)](const Reply& reply)
                    mutable noexcept
                {
                    // Snapshot before erasing the sole Scoped request owner:
                    // immediate replies execute this continuation inline.
                    Reply snapshot{reply};
                    owner->requests.erase(token);
                    ++owner->posted_callbacks;

                    struct PostedCall final
                    {
                        std::shared_ptr<Control> owner;
                        std::optional<Handler> handler;
                        Reply reply;
                        bool settled{false};

                        void run(bool compensation_only) noexcept
                        {
                            if (settled || !handler)
                                lux::render::renderFatal(
                                    "Owner reply trampoline settled twice"
                                );
                            settled = true;
                            {
                                auto invoke = std::move(*handler);
                                handler.reset();
                                invoke(reply, compensation_only);
                            }
                            --owner->posted_callbacks;
                        }
                    };

                    auto call = std::make_shared<PostedCall>(PostedCall{
                        owner,
                        std::move(handler),
                        std::move(snapshot),
                        false
                    });
                    const bool posted = post && post(
                        [call]() noexcept
                        { call->run(call->owner->abandoning); }
                    );
                    if (!posted)
                    {
                        // Executor generation disappeared before the phase
                        // hop. Business continuation is no longer legal, but
                        // the reply's raw owner still crossed the protocol.
                        call->run(/*compensation_only=*/true);
                    }
                }
            );
        }

        [[nodiscard]] std::size_t pending() const noexcept
        {
            return control_->requests.size() + control_->posted_callbacks;
        }

        /// Precondition: the AsyncScope owning the handler's completion path is
        /// stopped and joined.  Already-posted callbacks remain pending and run
        /// exactly once in compensation-only mode.
        void abandon() noexcept
        {
            control_->abandoning = true;
            control_->requests.clear();
        }

    private:
        struct Control final
        {
            std::unordered_map<
                std::uint64_t,
                lux::render::ScopedRenderRequest<Reply>> requests;
            std::uint64_t next_token{1};
            std::size_t   posted_callbacks{0};
            bool          abandoning{false};
        };

        TryPostToMain            post_main_;
        std::shared_ptr<Control> control_;
    };
}
