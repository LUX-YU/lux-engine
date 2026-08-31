#pragma once
// ============================================================================
//  RenderTaskScheduler.hpp — TEST-ONLY cooperative scheduler for RenderTask
//  coroutines. RELOCATED out of the production render API alongside RenderTask.hpp
//  (生产侧如今是 trySyncCall/awaitAllReady;sender 适配同为零生产消费者的占位);
//  this drives the render feature tests' coroutine harnesses.
//
//  Runs a RenderTask<void> inside the standard frame lifecycle:
//    beginFrame → resume coroutine → submitFrame → waitAndPumpReplies
//
//  The coroutine communicates frame boundaries via `co_await yield_frame()`.
//  When the coroutine awaits a RenderRequest, the reply callback stashes the
//  coroutine handle via g_pending_coro_resume; the scheduler resumes it after the
//  next beginFrame() so the coroutine can safely push commands.
// ============================================================================

#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include "RenderTask.hpp" // relocated beside this header (test-only)

#include <cassert>
#include <coroutine>
#include <functional>
#include <utility>

namespace lux::render
{
    /// Test-coroutine adapter. Production keeps the structured backpressure
    /// result and retries at a main-thread safe point; these probes run below
    /// saturation and convert rejection into a settled failure request.
    template <class Reply> [[nodiscard]] RenderRequest<Reply> requireUploadAccepted(UploadSubmitResult<Reply> submitted)
    {
        if (!submitted)
            return RenderRequestFactory<Reply>::makeImmediateFailure({});
        return std::move(submitted.value());
    }

    class RenderTaskScheduler
    {
    public:
        explicit RenderTaskScheduler(RenderProgramSession& session) : session_(session)
        {
        }

        RenderTaskScheduler(
            RenderProgramSession& frame,
            RenderControlSession& control,
            RenderUploadSession* upload = nullptr
        ) noexcept
            : session_(frame), control_(&control), upload_(upload)
        {
        }

        /// Run a coroutine task to completion, pumping frames as needed.
        ///
        /// @param task      The coroutine to drive.
        /// @param on_frame  Called each frame after submitFrame + pumpReplies.
        ///                  Return false to abort the loop (e.g. window close).
        template <typename PerFrameFn> void run(RenderTask<void> task, PerFrameFn&& on_frame)
        {
            // Install the deferred-resume slot so reply callbacks stash
            // coroutine handles instead of resuming them directly.
            std::coroutine_handle<> pending{nullptr};
            bool waiting_for_reply = false;
            g_pending_coro_resume = &pending;
            g_waiting_render_reply = &waiting_for_reply;

            // The task starts lazy (initial_suspend = always).
            // Begin the first frame and kick off the coroutine.
            session_.beginFrame();
            task.resume();

            while (!task.done())
            {
                // The coroutine has suspended — either at yield_frame()
                // or at co_await RenderRequest (waiting for a reply).
                session_.trySubmitFrame();
                session_.waitAndPumpReplies();
                if (control_)
                    control_->pumpReplies();
                if (upload_)
                    upload_->pumpReplies();

                // Per-frame callback (window poll, camera update, etc.)
                if (!on_frame(session_))
                    break;

                if (task.done())
                    break;

                // Start the next frame.
                session_.beginFrame();

                // If a reply callback stashed a pending handle during
                // pumpReplies, resume it now (inside an active frame).
                if (pending)
                {
                    auto h = std::exchange(pending, nullptr);
                    h.resume();
                }
                else if (!waiting_for_reply)
                {
                    // The coroutine yielded via yield_frame() and has
                    // no pending reply — resume it to continue work.
                    task.resume();
                }
                // Otherwise the coroutine is awaiting a request reply;
                // keep it suspended until a callback marks it ready.
            }

            // Final flush: submit any remaining commands.
            // Use non-blocking pump — if no frame was submitted (e.g. the
            // loop broke before beginFrame), there is no reply to wait for
            // and a blocking wait would deadlock.
            session_.trySubmitFrame();
            session_.pumpReplies();
            if (control_)
                control_->pumpReplies();
            if (upload_)
                upload_->pumpReplies();

            // Clean up the global slot.
            g_waiting_render_reply = nullptr;
            g_pending_coro_resume = nullptr;

            // Propagate exception if the coroutine ran to completion and threw.
            // If the loop was terminated early (e.g. window close), the
            // coroutine is still suspended — skip result() to avoid the
            // "handle_.done()" assertion.
            if (task.done())
                task.result();
        }

    private:
        RenderProgramSession& session_;
        RenderControlSession* control_{nullptr};
        RenderUploadSession* upload_{nullptr};
    };

} // namespace lux::render
