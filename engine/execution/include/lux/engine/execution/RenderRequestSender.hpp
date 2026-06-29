#pragma once
/**
 * @file RenderRequestSender.hpp
 * @brief stdexec/P2300 sender adapter over the render-comm `RenderRequest<T>`.
 *
 * `lux::exec::asSender(req)` turns a render reply (the bespoke future returned by
 * RenderSession commands) into an stdexec SENDER that completes with `set_value(T)`
 * when the reply arrives. This is the Phase-B keystone of the async-unification
 * effort: with it, render replies compose with the rest of the engine's async
 * vocabulary (`when_all`, `then`, `continues_on`, spawned on EngineExecutor's
 * `async_scope`) — Phase C rewrites the editor's hand-polled reply state machines
 * (ThumbnailService / PreviewScene / EditorTextureCache) as sender pipelines.
 *
 * Mechanics: `RenderRequest<T>` is `shared_ptr<State{value, continuation, ready}>`.
 * `.then(fn)` fires the continuation ON THE MAIN THREAD during
 * `RenderSession::pumpReplies()` (the render thread only writes the reply packet +
 * signals a CV). So this sender's `set_value` naturally lands on the main thread —
 * the same thread as EngineExecutor::drainMain — with no extra machinery. The op's
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
 *  4. completion_signatures advertises set_value_t(T) ONLY — RenderRequest has no
 *     cancellation, so there is no set_stopped path (do not add one; timeouts live
 *     at the Phase-C consumer level via when_any with a timeout sender).
 *  5. start() is noexcept but then() may bad_alloc storing the std::function →
 *     std::terminate (consistent with the codebase treating bad_alloc as fatal).
 *
 * Confinement: including this header pulls <stdexec/execution.hpp> — an OPT-IN
 * that requires MSVC /permissive- /Zc:__cplusplus /Zc:preprocessor (and a
 * find_package(stdexec) link) at the INCLUDER. EngineExecutor.hpp stays
 * stdexec-free; only includers of THIS header opt into stdexec.
 */

#include <cassert>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include <lux/engine/render/comm/client/RenderRequest.hpp>

namespace lux::exec
{
    namespace ex = stdexec;   // only ::stdexec is used here; the ::exec (extension)
                              // namespace — shadowed by lux::exec — is NOT referenced.

    namespace detail
    {
        /// Sender that completes with set_value(T) when @p req_ resolves.
        template <class T>
        struct render_request_sender
        {
            using sender_concept = ex::sender_t;
            using completion_signatures = ex::completion_signatures<ex::set_value_t(T)>;

            lux::render::RenderRequest<T> req_;

            // Plain sender: advertises NO completion scheduler (the adapter owns no
            // scheduler handle — completion lands on whoever pumps replies). empty_env
            // keeps stdexec::get_env(sender) well-formed for algorithms like when_all.
            ex::empty_env get_env() const noexcept { return {}; }

            template <class Rcvr>
            struct _op
            {
                using operation_state_concept = ex::operation_state_t;

                lux::render::RenderRequest<T> req_;   // own copy → State outlives the wait
                Rcvr                          rcvr_;

                void start() & noexcept
                {
                    // MUST be the last statement: for an already-ready request this
                    // completes (and may destroy *this) SYNCHRONOUSLY. The pending
                    // case parks the continuation; it fires later during pumpReplies.
                    req_.then([this](const T& v) noexcept
                              { ex::set_value(std::move(rcvr_), v); });
                }
            };

            template <class Rcvr>
            _op<std::decay_t<Rcvr>> connect(Rcvr&& r) const
            {
                return _op<std::decay_t<Rcvr>>{ req_, std::forward<Rcvr>(r) };
            }
        };
    } // namespace detail

    /// Adapt a render reply into an stdexec sender completing with set_value(T).
    /// Precondition: @p req is valid() (produced by RenderRequestFactory). See the
    /// file header for the single-continuation + synchronous-completion contracts.
    template <class T>
    [[nodiscard]] detail::render_request_sender<T>
    asSender(lux::render::RenderRequest<T> req)
    {
        assert(req.valid() &&
               "asSender: RenderRequest has no shared state (default-constructed)");
        return detail::render_request_sender<T>{ std::move(req) };
    }

} // namespace lux::exec
