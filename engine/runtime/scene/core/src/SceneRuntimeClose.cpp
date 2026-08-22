#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/scene/SceneRuntimeCloseSender.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <cstdlib>
#include <thread>
#include <utility>

namespace lux::runtime
{
    SceneCloseReport SceneRuntime::advanceClose() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            std::abort();

        const auto report = [this](
            ESceneCloseStatus status,
            ESceneCloseError error)
        {
            return SceneCloseReport{.status = status, .error = error};
        };

        if (closed_)
            return report(
                ESceneCloseStatus::ALREADY_CLOSED,
                ESceneCloseError::NONE);
        if (close_advancing_)
        {
            close_progress_pending_ = true;
            return report(
                ESceneCloseStatus::RETRY_REQUIRED,
                ESceneCloseError::NONE);
        }

        close_advancing_ = true;
        close_progress_pending_ = false;
        const auto result = [&]() noexcept -> SceneCloseReport
        {
            closing_ = true;
            live_ = false;
            state_ = ESceneRuntimeState::CLOSING;

                if (schedule_)
                {
                    schedule_->requestClose(lux::ecs::SystemCloseProgressSink{
                        this,
                        [](void* context) noexcept
                        {
                            static_cast<SceneRuntime*>(context)->
                                requestCloseProgress();
                        }});
                }

                // Domain preparation can retain ContentBlob leases while its
                // completion is still in the scene-owned scope. Stop that
                // admission before waiting for Section retirement, while all
                // systems are still installed to adopt cancellation/late
                // completions. The terminal scope callback is the single
                // external wake that lets loader close resume after those
                // leases have been released.
                if (async_scope_ && !async_scope_close_started_)
                {
                    async_scope_close_started_ = true;
                    lux::exec::detail::subscribeScopeClose(
                        *async_scope_,
                        [this]() noexcept
                        {
                            async_scope_closed_ = true;
                            requestCloseProgress();
                        });
                }

                if (schedule_)
                {
                    auto schedule_close = schedule_->closeState();
                    if (!schedule_close.valid)
                    {
                        return report(
                            ESceneCloseStatus::RETRY_REQUIRED,
                            ESceneCloseError::SCHEDULE_CLOSE_REJECTED);
                    }
                    if (!schedule_close.complete &&
                        schedule_close.owner_work_pending)
                    {
                        schedule_->tick(0.f);
                        schedule_close = schedule_->closeState();
                        if (!schedule_close.complete &&
                            schedule_close.owner_work_pending)
                        {
                            close_progress_pending_ = true;
                        }
                    }
                    if (!schedule_close.complete)
                    {
                        return report(
                            ESceneCloseStatus::RETRY_REQUIRED,
                            ESceneCloseError::NONE);
                    }
                }

                if (async_scope_ && !async_scope_closed_)
                {
                    return report(
                        ESceneCloseStatus::RETRY_REQUIRED,
                        ESceneCloseError::NONE);
                }

            // A queued owner follow-up still captures this SceneRuntime. A
            // different completion may have made every domain quiescent in
            // the meantime, but terminal delivery must wait until that task
            // has run and released the borrow.
            if (close_followup_queued_)
            {
                return report(
                    ESceneCloseStatus::RETRY_REQUIRED,
                    ESceneCloseError::NONE);
            }

            startup_sections_ = nullptr;
            entity_section_loader_ = nullptr;
            schedule_.reset();
            services_.reset();
            persistent_entities_.reset();
            world_.reset();
            extension_module_leases_.clear();
            state_ = ESceneRuntimeState::CLOSED;
            closed_ = true;
            return report(
                ESceneCloseStatus::CLOSED,
                ESceneCloseError::NONE);
        }();

        close_advancing_ = false;
        const bool needs_followup =
            !result.terminal() && close_progress_pending_;
        close_progress_pending_ = false;
        if (needs_followup && !close_waiters_.empty())
            scheduleCloseFollowup();
        if (result.terminal())
            finishClose(result);
        return result;
    }

    void SceneRuntime::requestCloseProgress() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            std::abort();
        if (!closing_)
            return;
        if (close_advancing_)
        {
            close_progress_pending_ = true;
            return;
        }
        (void)advanceClose();
    }

    void SceneRuntime::scheduleCloseFollowup() noexcept
    {
        if (closed_ || close_followup_queued_)
            return;
        close_followup_queued_ = true;
        async_.mainThreadMailbox().enqueue(
            [this]() noexcept
            {
                if (std::this_thread::get_id() != owner_thread_ ||
                    !close_followup_queued_)
                {
                    std::abort();
                }
                close_followup_queued_ = false;
                requestCloseProgress();
            });
    }

    SceneRuntimeCloseSender SceneRuntime::closeAsync() noexcept
    {
        return SceneRuntimeCloseSender{*this};
    }

    void detail::subscribeSceneClose(
        SceneRuntime& runtime,
        lux::cxx::move_only_function<void(SceneCloseReport)> completion)
        noexcept
    {
        runtime.subscribeClose(std::move(completion));
    }

    void SceneRuntime::subscribeClose(
        lux::cxx::move_only_function<void(SceneCloseReport)> completion)
        noexcept
    {
        if (!completion)
            return;
        if (std::this_thread::get_id() != owner_thread_)
            std::abort();
        subscribeCloseOnMain(std::move(completion));
    }

    void SceneRuntime::subscribeCloseOnMain(
        lux::cxx::move_only_function<void(SceneCloseReport)> completion)
        noexcept
    {
        if (!completion)
            return;
        if (closed_)
        {
            completion(advanceClose());
            return;
        }
        close_waiters_.push_back(std::move(completion));
        (void)advanceClose();
    }

    void SceneRuntime::finishClose(SceneCloseReport report) noexcept
    {
        if (!report.terminal() || close_waiters_.empty())
            return;
        auto waiters = std::move(close_waiters_);
        close_waiters_.clear();
        for (auto& waiter : waiters)
            if (waiter)
                waiter(report);
    }

    bool SceneRuntime::failBringUp() noexcept
    {
        const auto report = advanceClose();
        requireTerminalCloseBeforeDestruction(report, "bring-up rollback");
        return false;
    }
}
