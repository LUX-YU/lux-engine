#pragma once
// ============================================================================
//  RenderRequest.hpp — Async result primitive for render commands with replies
//
//  A lightweight future-like handle returned by RenderSession methods. Compose
//  it with stdexec senders via lux::exec::asSender (engine/execution); the older
//  coroutine driver (RenderTask / co_await) was relocated to test-only support
//  (modules/function/render/test/RenderTask.hpp) once senders became THE async
//  vocabulary, so this primitive is coroutine-free.
// ============================================================================

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/comm/FrameProgram.hpp>

#include <cassert>
#include <cstring>
#include <functional>
#include <memory>

namespace lux::render
{
    // =========================================================================
    //  RenderRequest<T> — client-facing async result
    // =========================================================================
    template <typename T>
    class RenderRequest
    {
    public:
        RenderRequest() = default;

        [[nodiscard]] bool isReady() const noexcept { return state_ && state_->ready; }
        explicit operator bool() const noexcept { return isReady(); }

        /// True iff this request was produced by RenderRequestFactory (has shared
        /// state), as opposed to a default-constructed handle. Distinct from
        /// isReady() (which is false for BOTH a pending and a stateless request).
        /// then()/result() have undefined behaviour on a !valid() request.
        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

        /// Access the result. Undefined if !isReady().
        [[nodiscard]] const T& result() const noexcept
        {
            assert(isReady());
            return state_->value;
        }

        /// Register a continuation called when the result arrives.
        /// If already ready, the continuation is invoked immediately.
        template <typename F>
        void then(F&& fn)
        {
            assert(state_);
            if (state_->ready)
            {
                fn(state_->value);
            }
            else
            {
                state_->continuation = std::forward<F>(fn);
            }
        }

        /// Detach the continuation so a reply that arrives later is DROPPED. The
        /// shared state survives (the reply store still holds it and will memcpy
        /// the payload in), but no continuation runs. Use when the continuation
        /// captures objects that are about to be destroyed (e.g. a render bridge +
        /// its context): without this, a reply pumped after their destruction —
        /// possibly by an UNRELATED pumpReplies elsewhere, since replies share one
        /// queue — calls into freed memory. The created render object (if any) is
        /// then reclaimed by the reap / scene-destruction fallback, not here.
        /// Idempotent; no-op on a stateless or already-fired request.
        void cancel() noexcept
        {
            if (state_)
                state_->continuation = nullptr;
        }

    private:
        struct State
        {
            T value{};
            std::function<void(const T&)> continuation{};
            bool ready{false};
        };

        std::shared_ptr<State> state_;

        explicit RenderRequest(std::shared_ptr<State> s) : state_(std::move(s)) {}

        template <typename U, std::size_t A>
        friend struct RenderRequestFactory;
    };

    // =========================================================================
    //  RenderRequestFactory — internal helper used by RenderSession
    // =========================================================================
    template <typename Reply, std::size_t ReplyAlignment = 64>
    struct RenderRequestFactory
    {
        using Packet   = FrameReplies<ReplyAlignment>;
        using Callback = std::function<void(const Packet&, const ReplyRecord&)>;

        struct Result
        {
            RenderRequest<Reply> request;
            Callback             callback;
        };

        static Result make()
        {
            auto state = std::make_shared<typename RenderRequest<Reply>::State>();

            auto callback = [state](const Packet& pkt, const ReplyRecord& rec)
            {
                assert(rec.payload_size == sizeof(Reply));
                Reply value{};
                std::memcpy(&value, pkt.payload.data() + rec.payload_offset, sizeof(Reply));

                state->value = value;
                state->ready = true;
                if (state->continuation)
                {
                    state->continuation(state->value);
                }
            };
            return { RenderRequest<Reply>(state), std::move(callback) };
        }

        /// Construct a pre-completed RenderRequest carrying @p value. No wire
        /// dispatch occurs; any subsequent .then() runs the continuation
        /// synchronously. Used for early-exit failures (e.g. unsupported
        /// pixel format) where the caller wants the standard reply shape.
        static RenderRequest<Reply> makeImmediate(Reply value)
        {
            auto state = std::make_shared<typename RenderRequest<Reply>::State>();
            state->value = std::move(value);
            state->ready = true;
            return RenderRequest<Reply>(state);
        }
    };

} // namespace lux::render
