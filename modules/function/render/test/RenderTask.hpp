#pragma once
// ============================================================================
//  RenderTask.hpp — TEST-ONLY C++20 coroutine support for render client tests.
//
//  RELOCATED out of the production render API (modules/.../comm/client/) in the
//  async-unification effort. 生产侧的等待词汇如今是 trySyncCall/awaitAllReady
//  与 .then 连续体(stdexec 适配 asSender 同为零生产消费者的能力占位)。The
//  coroutine path was only ever a test driver (no production consumer), so it
//  lives here as test scaffolding. Includes:
//    - thread_local resume slots + operator co_await(RenderRequest<T>) (moved out
//      of RenderRequest.hpp so the production reply primitive is coroutine-free)
//    - co_await RenderRequest<T>  — suspend until server reply arrives
//    - co_await yield_frame()     — suspend to end the current frame
//    - RenderTask<T> / co_return  — the coroutine return type (driven by
//      RenderTaskScheduler, beside this header)
// ============================================================================

#include <cassert>
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include <lux/engine/function/render/client/RenderRequest.hpp>

namespace lux::render
{
    // =====================================================================
    //  RenderRequest<T> coroutine support (moved from RenderRequest.hpp)
    //
    //  When the reply arrives (during pumpReplies), the coroutine handle is
    //  stored but NOT immediately resumed. The RenderTaskScheduler checks for
    //  pending handles and resumes them after beginFrame() so the coroutine can
    //  safely push commands.
    // =====================================================================

    /// Shared queue where reply callbacks enqueue coroutine handles for deferred
    /// resumption by the scheduler. Set by RenderTaskScheduler before running.
    thread_local inline std::coroutine_handle<>* g_pending_coro_resume = nullptr;

    /// Flag indicating the currently running coroutine is suspended waiting for a
    /// RenderRequest reply. Managed by RenderTaskScheduler.
    thread_local inline bool* g_waiting_render_reply = nullptr;

    template <typename T> auto operator co_await(RenderRequest<T>& req)
    {
        struct RenderRequestAwaiter
        {
            RenderRequest<T>& req_;

            bool await_ready() const noexcept
            {
                return req_.isReady();
            }

            void await_suspend(std::coroutine_handle<> h)
            {
                if (g_waiting_render_reply)
                    *g_waiting_render_reply = true;
                req_.then([h](const T&) mutable {
                    if (g_waiting_render_reply)
                        *g_waiting_render_reply = false;
                    // Don't resume immediately — stash for the scheduler.
                    if (g_pending_coro_resume)
                        *g_pending_coro_resume = h;
                }
                );
            }

            const T& await_resume() const noexcept
            {
                return req_.tryResult()->get();
            }
        };
        return RenderRequestAwaiter{req};
    }

    template <typename T> auto operator co_await(RenderRequest<T>&& req)
    {
        struct RenderRequestAwaiter
        {
            RenderRequest<T> req_;

            bool await_ready() const noexcept
            {
                return req_.isReady();
            }

            void await_suspend(std::coroutine_handle<> h)
            {
                if (g_waiting_render_reply)
                    *g_waiting_render_reply = true;
                req_.then([h](const T&) mutable {
                    if (g_waiting_render_reply)
                        *g_waiting_render_reply = false;
                    if (g_pending_coro_resume)
                        *g_pending_coro_resume = h;
                }
                );
            }

            const T& await_resume() const noexcept
            {
                return req_.tryResult()->get();
            }
        };
        return RenderRequestAwaiter{std::move(req)};
    }

    // Forward declarations
    template <typename T> class RenderTask;

    // =====================================================================
    //  FrameYield — awaitable that suspends the coroutine at a frame boundary
    // =====================================================================
    struct FrameYield
    {
        bool await_ready() const noexcept
        {
            return false;
        }
        void await_suspend(std::coroutine_handle<>) const noexcept
        {
        }
        void await_resume() const noexcept
        {
        }
    };

    /// Convenience factory — usage: `co_await yield_frame();`
    inline FrameYield yield_frame() noexcept
    {
        return {};
    }

    // =====================================================================
    //  RenderTask<T> — coroutine return type
    // =====================================================================
    template <typename T = void> class RenderTask
    {
    public:
        struct promise_type
        {
            std::optional<T> value{};
            std::exception_ptr exception{};
            std::coroutine_handle<> continuation{}; // who to resume when we finish

            RenderTask get_return_object()
            {
                return RenderTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            auto final_suspend() noexcept
            {
                // If there is a continuation (parent coroutine), resume it.
                struct FinalAwaiter
                {
                    bool await_ready() const noexcept
                    {
                        return false;
                    }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                    {
                        if (h.promise().continuation)
                            return h.promise().continuation;
                        return std::noop_coroutine();
                    }
                    void await_resume() noexcept
                    {
                    }
                };
                return FinalAwaiter{};
            }

            template <typename U> void return_value(U&& val)
            {
                value.emplace(std::forward<U>(val));
            }

            void unhandled_exception()
            {
                exception = std::current_exception();
            }
        };

        // -- Constructors / move --
        RenderTask() = default;
        explicit RenderTask(std::coroutine_handle<promise_type> h) : handle_(h)
        {
        }
        ~RenderTask()
        {
            if (handle_)
                handle_.destroy();
        }

        RenderTask(const RenderTask&) = delete;
        RenderTask& operator=(const RenderTask&) = delete;

        RenderTask(RenderTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr))
        {
        }
        RenderTask& operator=(RenderTask&& o) noexcept
        {
            if (this != &o)
            {
                if (handle_)
                    handle_.destroy();
                handle_ = std::exchange(o.handle_, nullptr);
            }
            return *this;
        }

        // -- Query --
        [[nodiscard]] bool done() const noexcept
        {
            return !handle_ || handle_.done();
        }

        /// Resume until the coroutine suspends or finishes.
        void resume() const
        {
            assert(handle_ && !handle_.done());
            handle_.resume();
        }

        /// Get the result (only valid after done()).
        [[nodiscard]] T result() const
        {
            assert(handle_ && handle_.done());
            auto& p = handle_.promise();
            if (p.exception)
                std::rethrow_exception(p.exception);
            assert(p.value.has_value());
            return *p.value;
        }

        // -- co_await support (for nesting RenderTask inside another) --
        bool await_ready() const noexcept
        {
            return done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller)
        {
            handle_.promise().continuation = caller;
            return handle_; // symmetric transfer into this task
        }

        T await_resume()
        {
            return result();
        }

        /// Access the raw coroutine handle
        [[nodiscard]] std::coroutine_handle<promise_type> handle() const noexcept
        {
            return handle_;
        }

    private:
        std::coroutine_handle<promise_type> handle_{nullptr};
    };

    // =====================================================================
    //  RenderTask<void> specialization
    // =====================================================================
    template <> class RenderTask<void>
    {
    public:
        struct promise_type
        {
            std::exception_ptr exception{};
            std::coroutine_handle<> continuation{};

            RenderTask get_return_object()
            {
                return RenderTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept
            {
                return {};
            }

            auto final_suspend() noexcept
            {
                struct FinalAwaiter
                {
                    bool await_ready() const noexcept
                    {
                        return false;
                    }
                    std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                    {
                        if (h.promise().continuation)
                            return h.promise().continuation;
                        return std::noop_coroutine();
                    }
                    void await_resume() noexcept
                    {
                    }
                };
                return FinalAwaiter{};
            }

            void return_void()
            {
            }
            void unhandled_exception()
            {
                exception = std::current_exception();
            }
        };

        RenderTask() = default;
        explicit RenderTask(std::coroutine_handle<promise_type> h) : handle_(h)
        {
        }
        ~RenderTask()
        {
            if (handle_)
                handle_.destroy();
        }

        RenderTask(const RenderTask&) = delete;
        RenderTask& operator=(const RenderTask&) = delete;

        RenderTask(RenderTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr))
        {
        }
        RenderTask& operator=(RenderTask&& o) noexcept
        {
            if (this != &o)
            {
                if (handle_)
                    handle_.destroy();
                handle_ = std::exchange(o.handle_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] bool done() const noexcept
        {
            return !handle_ || handle_.done();
        }

        void resume() const
        {
            assert(handle_ && !handle_.done());
            handle_.resume();
        }

        void result() const
        {
            assert(handle_ && handle_.done());
            if (handle_.promise().exception)
                std::rethrow_exception(handle_.promise().exception);
        }

        bool await_ready() const noexcept
        {
            return done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller)
        {
            handle_.promise().continuation = caller;
            return handle_;
        }

        void await_resume()
        {
            result();
        }

        [[nodiscard]] std::coroutine_handle<promise_type> handle() const noexcept
        {
            return handle_;
        }

    private:
        std::coroutine_handle<promise_type> handle_{nullptr};
    };

} // namespace lux::render
