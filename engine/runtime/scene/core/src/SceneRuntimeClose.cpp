#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/scene/SceneRuntimeCloseSender.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/StartupSectionSystem.hpp>
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
    namespace
    {
        struct EntitySectionCloseState final
        {
            std::size_t waiting_sections{0u};
            std::size_t staging_sections{0u};
            std::size_t armed_sections{0u};
            std::size_t active_sections{0u};
            std::size_t allocated_slots{0u};
            std::size_t free_slots{0u};
            std::size_t section_mappings{0u};
            std::size_t blob_bytes{0u};
            std::size_t blob_allocations{0u};
            std::size_t blob_lookups{0u};

            friend bool operator==(
                const EntitySectionCloseState&,
                const EntitySectionCloseState&) = default;
        };

        [[nodiscard]] EntitySectionCloseState sectionCloseState(
            const entity_scene::EntitySectionLoaderSystem& loader) noexcept
        {
            const auto snapshot = loader.snapshot();
            return {
                .waiting_sections = snapshot.waiting_sections,
                .staging_sections = snapshot.staging_sections,
                .armed_sections = snapshot.armed_sections,
                .active_sections = snapshot.active_sections,
                .allocated_slots = snapshot.allocated_slots,
                .free_slots = snapshot.free_slots,
                .section_mappings = snapshot.section_mappings,
                .blob_bytes = snapshot.blobs.current_bytes,
                .blob_allocations = snapshot.blobs.allocation_count,
                .blob_lookups = snapshot.blobs.lookup_entries,
            };
        }
    }

    SceneCloseReport SceneRuntime::advanceClose() noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            std::abort();

        const auto report = [this](
            ESceneCloseStatus status,
            ESceneCloseError error)
        {
            return SceneCloseReport{
                .status = status,
                .error = error,
                .integration_closed = integration_closed_};
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

                if (integration_ && !integration_closed_)
                {
                    const auto status = integration_->close();
                    if (status ==
                            ESceneIntegrationCloseStatus::RETRY_REQUIRED ||
                        status == ESceneIntegrationCloseStatus::FAILED)
                    {
                        return report(
                            ESceneCloseStatus::RETRY_REQUIRED,
                            ESceneCloseError::INTEGRATION_CLOSE_REJECTED);
                    }
                    integration_closed_ = true;
                }
                else if (!integration_)
                {
                    integration_closed_ = true;
                }

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

                if (startup_sections_ && !startup_close_started_)
                {
                    startup_close_started_ = true;
                    auto close = startup_sections_->closeAsync()
                        | stdexec::then(
                              [this]() noexcept
                              {
                                  startup_closed_ = true;
                                  requestCloseProgress();
                              });
                    ::experimental::execution::start_detached(
                        std::move(close));
                }
                else if (!startup_sections_)
                {
                    startup_closed_ = true;
                }
                if (schedule_ && !startup_closed_)
                {
                    const bool has_loader =
                        entity_section_loader_ != nullptr;
                    const auto before = has_loader
                        ? sectionCloseState(*entity_section_loader_)
                        : EntitySectionCloseState{};
                    schedule_->tick(
                        0.f,
                        lux::ecs::kPhaseSceneLoading);
                    if (!startup_closed_ && has_loader &&
                        startup_sections_->state() ==
                            entity_scene::EEntitySceneState::CLOSING &&
                        before != sectionCloseState(
                            *entity_section_loader_))
                    {
                        // The barrier made synchronous owner progress after
                        // StartupSectionSystem had already run. One more
                        // loading safe point is therefore actionable without
                        // waiting for an external completion. A stable owner
                        // snapshot never requeues itself, so this cannot spin.
                        close_progress_pending_ = true;
                    }
                }
                if (!startup_closed_)
                {
                    return report(
                        ESceneCloseStatus::RETRY_REQUIRED,
                        ESceneCloseError::NONE);
                }

                if (entity_section_loader_ &&
                    !entity_loader_close_started_)
                {
                    entity_loader_close_started_ = true;
                    auto close = entity_section_loader_->closeAsync()
                        | stdexec::then(
                              [this]() noexcept
                              {
                                  entity_loader_closed_ = true;
                                  requestCloseProgress();
                              });
                    ::experimental::execution::start_detached(
                        std::move(close));
                }
                else if (!entity_section_loader_)
                {
                    entity_loader_closed_ = true;
                }
                if (schedule_ && !entity_loader_closed_)
                {
                    schedule_->tick(0.f);
                    const auto owner_state = schedule_->closeState();
                    if (!entity_loader_closed_ && owner_state.valid &&
                        owner_state.owner_work_pending)
                    {
                        // Section deactivation can leave immutable blob
                        // leases in consumers from later phases. Keep every
                        // system installed and run the complete dt=0 owner
                        // safe point so those consumers can retire them.
                        // Every system reports whether another bounded owner
                        // granule is executable; external-only waits wake via
                        // their retained close-progress sink or scene scope.
                        close_progress_pending_ = true;
                    }
                }
                if (!entity_loader_closed_)
                {
                    return report(
                        ESceneCloseStatus::RETRY_REQUIRED,
                        ESceneCloseError::NONE);
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
            entity_scene_catalog_ = nullptr;
            schedule_.reset();
            integration_.reset();
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
