#pragma once
/**
 * @file RenderRequestSender.hpp
 * @brief stdexec/P2300 sender adapter over the render-comm `RenderRequest<T>`.
 *
 * `lux::runtime::render_detail::asSender(req)` adapts the render RPC primitive
 * inside the runtime render integration.  It is deliberately not an execution
 * public API: AsyncRuntime has no render dependency.
 * stdexec sender. A normal reply completes with `set_value(T)`; a generic
 * dispatch failure completes with `set_error(RenderError)`. No exception-based
 * error channel is introduced.
 *
 * Mechanics: `RenderRequest<T>` is `shared_ptr<State{value, continuation, ready}>`.
 * `.then(fn)` fires the continuation ON THE MAIN THREAD during
 * `RenderFrameSession::pumpReplies()` (the render thread only writes the reply packet +
 * signals a CV). So this sender's `set_value` naturally lands on the main thread —
 * the same thread as AsyncRuntime::drainMainThreadCompletions — with no extra machinery. The op's
 * `start()` simply registers `req.then(set_value)`.
 *
 * ── FOOTGUNS (read before using) ─────────────────────────────────────────────
 *  1. SINGLE CONTINUATION (sharpest): RenderRequest has ONE continuation slot;
 *     then() silently OVERWRITES. So: do NOT also call .then()/co_await on a
 *     request you've adapted, and do NOT asSender the SAME request twice — the
 *     dropped continuation never completes (a sync_wait hang / an async_scope
 *     leak). All copies of a RenderRequest share the one State + one slot, so the
 *     by-value copy below does not save you. The adapter cannot detect this.
 *  2. NULL / default request: asSender asserts `req.valid()`. A default-constructed
 *     RenderRequest has no state and then() would null-deref (mirrors result()'s
 *     "undefined if !isReady()" precondition style).
 *  3. SYNCHRONOUS SELF-DESTRUCTION: for an already-ready request (makeImmediate /
 *     already-pumped) then() invokes the continuation SYNCHRONOUSLY inside start(),
 *     so set_value runs and may DESTROY this op before start() returns. Hence
 *     `req_.then(...)` is the LAST statement of start() and nothing touches op
 *     state after it. (The pending path is also safe: the factory callback keeps
 *     its own shared_ptr<State> ref alive across the continuation call.)
 *  4. There is intentionally no set_stopped path yet. `RenderRequest::cancel()`
 *     detaches the CLIENT continuation; it does not cancel a command already
 *     published to the render server. Owner shutdown must keep pumping replies
 *     (AsyncScope::close(progress)) until the operation settles. Pretending detach
 *     is server cancellation would leak the created GPU object.
 *  5. start() is noexcept but then() may bad_alloc storing the std::function →
 *     std::terminate (consistent with the codebase treating bad_alloc as fatal).
 *
 * Confinement: including this header pulls <stdexec/execution.hpp> — an OPT-IN
 * that requires MSVC /permissive- /Zc:__cplusplus /Zc:preprocessor (and a
 * find_package(stdexec) link) at the INCLUDER. AsyncRuntime.hpp stays
 * stdexec-free; only includers of THIS header opt into stdexec.
 */

#include <cassert>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include <lux/engine/function/render/client/RenderRequest.hpp>

namespace lux::runtime::render_detail
{
    namespace ex = stdexec;   // only ::stdexec is used here; the ::exec (extension)
                              // namespace — shadowed by lux::exec — is NOT referenced.

    namespace sender_detail
    {
        /// Sender that completes with value or typed render-dispatch error.
        template <class T>
        struct render_request_sender
        {
            using sender_concept = ex::sender_t;
            using completion_signatures = ex::completion_signatures<
                ex::set_value_t(T),
                ex::set_error_t(lux::render::RenderError)>;

            lux::render::RenderRequest<T> req_;

            // Plain sender: advertises NO completion scheduler (the adapter owns no
            // scheduler handle — completion lands on whoever pumps replies). env<>
            // keeps stdexec::get_env(sender) well-formed for algorithms like when_all.
            ex::env<> get_env() const noexcept { return {}; }

            template <class Rcvr>
            struct _op
            {
                using operation_state_concept = ex::operation_state_t;

                // The operation state owns the sole continuation lexically. If a
                // never-started/abandoned op is destroyed, RAII detaches it.
                lux::render::ScopedRenderRequest<T> req_;
                Rcvr                                rcvr_;

                void start() & noexcept
                {
                    // MUST be the last statement: for an already-ready request this
                    // completes (and may destroy *this) SYNCHRONOUSLY. The pending
                    // case parks the continuation; it fires later during pumpReplies.
                    req_.then([this](const T& v) noexcept
                    {
                        if (req_.failed())
                        {
                            const auto error = req_.error();
                            ex::set_error(std::move(rcvr_), error);
                            return;
                        }
                        ex::set_value(std::move(rcvr_), v);
                    });
                }
            };

            template <class Rcvr>
            _op<std::decay_t<Rcvr>> connect(Rcvr&& r) const
            {
                return _op<std::decay_t<Rcvr>>{
                    lux::render::ScopedRenderRequest<T>{
                        lux::render::RenderRequest<T>{req_}},
                    std::forward<Rcvr>(r)};
            }
        };
    } // namespace sender_detail

    /// Adapt a render reply into an stdexec sender completing with value or
    /// RenderError.
    /// Precondition: @p req is valid() (produced by RenderRequestFactory). See the
    /// file header for the single-continuation + synchronous-completion contracts.
    template <class T>
    [[nodiscard]] sender_detail::render_request_sender<T>
    asSender(lux::render::RenderRequest<T> req)
    {
        assert(req.valid() &&
               "asSender: RenderRequest has no shared state (default-constructed)");
        return sender_detail::render_request_sender<T>{ std::move(req) };
    }

} // namespace lux::runtime::render_detail
