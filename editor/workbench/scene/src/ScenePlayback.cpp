#include <algorithm>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/ScenePlayback.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

namespace lux::editor::scene
{
    namespace
    {
        bool isBusy(const RunFailure& value) noexcept
        {
            const auto* control = std::get_if<ERunError>(&value.cause);
            return control && *control == ERunError::BUSY;
        }
        bool isBusy(const SceneEditError& value) noexcept
        {
            return value.code == ESceneEditError::BUSY || value.session == sessions::ESessionError::BUSY;
        }
        bool isBusy(sessions::ESessionError value) noexcept
        {
            return value == sessions::ESessionError::BUSY;
        }
        bool isBusy(desktop::EUiError value) noexcept
        {
            return value == desktop::EUiError::BUSY;
        }
        bool isBusy(const desktop::UiFailure& value) noexcept
        {
            return isBusy(value.code);
        }
        bool isBusy(lux::ui::EAttachmentError value) noexcept
        {
            return value == lux::ui::EAttachmentError::BUSY;
        }
        bool isBusy(const SceneViewFailure& value) noexcept
        {
            if (const auto* edit = std::get_if<SceneEditError>(&value.cause))
            {
                return isBusy(*edit);
            }
            if (const auto* run = std::get_if<RunFailure>(&value.cause))
            {
                return isBusy(*run);
            }
            if (const auto* ui = std::get_if<desktop::EUiError>(&value.cause))
            {
                return isBusy(*ui);
            }
            if (const auto* tree = std::get_if<lux::ui::EAttachmentError>(&value.cause))
            {
                return isBusy(*tree);
            }
            if (const auto* projection = std::get_if<ProjectionFailure>(&value.cause))
            {
                if (const auto* code = std::get_if<EProjectionError>(&projection->cause))
                {
                    return *code == EProjectionError::BUSY;
                }
                if (const auto* edit = std::get_if<SceneEditError>(&projection->cause))
                {
                    return isBusy(*edit);
                }
            }
            return false;
        }
        template <class Error> auto failure(std::string domain, const Error& error)
        {
            return cxx::unexpected(EditorFailure{
                isBusy(error) ? EEditorError::BUSY : EEditorError::SOURCE_FAILURE,
                std::move(domain),
                0,
                {},
                error
            });
        }
    } // namespace
    struct ScenePlayback::Impl final
    {
        struct Record final
        {
            StartRunId start;
            sessions::ContentStamp source;
            std::unique_ptr<StartRunOperation> preparing;
            std::optional<RunId> run;
            std::vector<lux::ui::PaneHandle> views;
            std::optional<StopTicket> stopping;
            bool stop_requested{};
            std::vector<StepTicket> steps;
            std::optional<EditorFailure> failure;
        };
        std::shared_ptr<sessions::SessionStore> sessions_;
        std::shared_ptr<RunStore> runs_;
        std::shared_ptr<const ProjectionEnvironment> environment_;
        lux::ui::Root& root_;
        desktop::UiRegistry& windows_;
        services::ServiceRegistry& services_;
        services::ServiceScope& scope_;
        const std::thread::id owner_{std::this_thread::get_id()};
        mutable bool dispatching_{};
        bool closing_{};
        std::uint64_t next_view_{1};
        std::vector<Record> records_;
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        Impl(
            std::shared_ptr<sessions::SessionStore> sessions,
            std::shared_ptr<RunStore> runs,
            std::shared_ptr<const ProjectionEnvironment> environment,
            lux::ui::Root& root,
            desktop::UiRegistry& windows,
            services::ServiceRegistry& services,
            services::ServiceScope& scope
        )
            : sessions_(std::move(sessions)), runs_(std::move(runs)), environment_(std::move(environment)), root_(root),
              windows_(windows), services_(services), scope_(scope)
        {
            records_.reserve(16);
        }
        ~Impl()
        {
            // Foreign source/result cleanup cannot admit another operation into dying records.
            dispatching_ = true;
        }
        [[nodiscard]] EditorResult<void> admission() const noexcept
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "playback.owner-thread"});
            }
            if (dispatching_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "playback.dispatch"});
            }
            return {};
        }
        EditorResult<StartRunId> play(sessions::ContentStamp);
        EditorResult<void> update(bool);
        EditorResult<void> stop(RunId);
        EditorResult<void> step(RunId);
        EditorResult<lux::ui::PaneHandle> showTool(lux::ui::PaneHandle, ESceneTool);
        EditorResult<void> requestClose() noexcept;
        bool settled() const noexcept;
        EditorResult<std::vector<RunPresentationInfo>> reports() const;
        EditorResult<void> acknowledgeFailure(StartRunId id);
        EditorResult<void> acknowledgeStep(StepTicket ticket);
        EditorResult<void> forgetViews(std::span<const lux::ui::PaneHandle> ids) noexcept;
        EditorResult<void> requestStop(RunId id) noexcept;
        EditorResult<lux::ui::PaneHandle> adopt(std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>& candidate)
        {
            auto* pane = candidate.get();
            auto mounted = root_.addSubPane(std::move(candidate));
            if (!mounted)
            {
                return failure("playback.view.mount", mounted.error());
            }
            auto identity = root_.identify(*pane);
            if (!identity)
            {
                return failure("playback.view.identity", identity.error());
            }
            return *identity;
        }
    };
    EditorResult<StartRunId> ScenePlayback::Impl::play(sessions::ContentStamp target)
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};

        const bool cannot_start = closing_ || records_.size() >= 16;
        if (cannot_start)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.admission"});
        }
        auto information = sessions_->describe(target.session);
        if (!information)
        {
            return failure("run.source", information.error());
        }
        if (information->current != target)
        {
            return failure("run.source", sessions::ESessionError::STALE_CONTENT);
        }
        auto key = sessions_->key<SceneSession>(target.session);
        if (!key)
        {
            return failure("run.source.kind", key.error());
        }
        auto author = sessions_->access<SceneSession>().read(*key);
        if (!author)
        {
            return failure("run.source.read", author.error());
        }
        auto captured = author->get().capture();
        if (!captured)
        {
            return failure("run.source.capture", captured.error());
        }
        RunConfiguration configuration;
        const auto& description = captured->configuration().scene->data();
        for (std::size_t i{}; i < description.systemCount(); ++i)
        {
            if (const auto system = description.systemAt(i);
                system.type() == lux::scene::builtinRenderSystemRegistration().type)
            {
                if (configuration.viewport.value)
                {
                    return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "run.viewport.ambiguous"});
                }
                configuration.viewport = system.instanceId();
            }
        }
        // The preparation owns the frozen author data and this exact environment, never a live Session.
        auto prepared = runs_->prepare(
            std::move(*captured),
            {environment_->components,
             environment_->simulation_systems,
             environment_->scene_systems,
             environment_->render_bindings,
             environment_->renderer,
             environment_->resources,
             environment_->assets},
            configuration
        );
        if (!prepared)
        {
            return failure("run.prepare", prepared.error());
        }
        const auto id = (*prepared)->id();
        records_.push_back({id, information->current, std::move(*prepared)});
        return id;
    }
    EditorResult<void> ScenePlayback::Impl::update(bool accept_prepared)
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};

        for (auto iterator = records_.begin(); iterator != records_.end();)
        {
            auto& record = *iterator;
            if (record.preparing)
            {
                if (closing_)
                {
                    record.preparing->cancel();
                }
                const bool defer_adoption = !record.preparing->ready() || !accept_prepared;
                if (defer_adoption)
                {
                    ++iterator;
                    continue;
                }
                auto adopted = runs_->adopt(*record.preparing);
                if (!adopted)
                {
                    const auto* control = std::get_if<ERunError>(&adopted.error().cause);
                    if (control && *control == ERunError::BUSY)
                    {
                        ++iterator;
                        continue;
                    }
                    record.failure = failure("run.adopt", adopted.error()).value();
                    record.preparing.reset();
                    ++iterator;
                    continue;
                }
                record.preparing.reset();
                record.run = *adopted;
                if (!closing_)
                {
                    const auto information = runs_->info(*adopted);
                    if (!information)
                    {
                        return failure("run.info", information.error());
                    }
                    const auto name =
                        "run-" + std::to_string(record.start.domain) + "-" + std::to_string(record.start.serial);
                    auto factory = windows_.snapshot().find(kSceneView.type);
                    if (!factory)
                    {
                        return failure("run.factory", factory.error());
                    }
                    auto candidate = windows_.create(
                        *factory,
                        scope_,
                        {root_.dispatcherRef(),
                         lux::ui::PaneId{name},
                         {},
                         {factory->descriptor().schema, {}},
                         views::ViewRestoreKey{name}}
                    );
                    if (!candidate)
                    {
                        record.failure = failure("run.view", candidate.error()).value();
                    }
                    else
                    {
                        auto& view = static_cast<SceneView&>(**candidate);
                        view.setTitle("Run (frozen author content)");
                        auto bound = view.rebindRun(*adopted, information->provenance.configuration.viewport);
                        if (!bound)
                        {
                            record.failure = failure("run.view.bind", bound.error()).value();
                        }
                        else
                        {
                            auto shown = adopt(*candidate);
                            if (!shown)
                            {
                                record.failure = shown.error();
                            }
                            else
                            {
                                record.views.push_back(*shown);
                            }
                        }
                    }
                }
            }
            const bool needs_stop = closing_ || record.stop_requested;
            const bool can_stop = record.run && !record.stopping;
            if (needs_stop && can_stop)
            {
                auto stopped = runs_->stop(*record.run);
                if (!stopped)
                {
                    return failure("run.stop", stopped.error());
                }
                record.stopping = *stopped;
            }
            // RunStore retains each original terminal result until the user's explicit Stop confirmation.
            // A frame must not manufacture acknowledgement merely because execution has finished.
            if (record.stopping && record.stopping->complete())
            {
                auto acknowledged = runs_->acknowledgeStop(*record.run);
                if (!acknowledged)
                {
                    // Retirement may finish after this frame's RunStore maintenance. Keep the
                    // result until that owner has adopted it; never drive the Runtime again here.
                    const auto* control = std::get_if<ERunError>(&acknowledged.error().cause);
                    if (control && *control == ERunError::BUSY)
                    {
                        ++iterator;
                        continue;
                    }
                    return failure("run.stop.acknowledge", acknowledged.error());
                }
                iterator = records_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        return {};
    }
    EditorResult<void> ScenePlayback::Impl::stop(RunId id)
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};

        auto record = std::ranges::find(records_, std::optional{id}, &Record::run);
        if (record == records_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "run.stop"});
        }
        const auto ids = record->views;
        auto prepared = windows_.prepareClose(root_, ids);
        if (!prepared)
        {
            return failure("run.views.prepare", prepared.error());
        }
        auto stopped = runs_->stop(id);
        if (!stopped)
        {
            return failure("run.stop", stopped.error());
        }
        record->stopping = *stopped;
        auto committed = root_.commit(*prepared);
        if (!committed)
        {
            return failure("run.views.close", committed.error());
        }
        record->views.clear();
        return {};
    }
    EditorResult<lux::ui::PaneHandle> ScenePlayback::Impl::showTool(lux::ui::PaneHandle source, ESceneTool kind)
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};

        if (closing_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "scene.tool.admission"});
        }
        auto source_group = shareSceneInteraction(root_, source);
        if (!source_group)
        {
            return failure("scene.tool.source", source_group.error());
        }
        const auto run = (*source_group)->run();
        auto candidate = createSceneTool(
            windows_,
            services_,
            scope_,
            root_,
            source,
            kind,
            lux::ui::PaneId{"scene-tool-" + std::to_string(next_view_++)}
        );
        if (!candidate)
        {
            return failure("scene.tool.create", candidate.error());
        }
        auto shown = adopt(*candidate);
        if (!shown)
        {
            return shown;
        }
        if (run)
        {
            auto owner = std::ranges::find(records_, run, &Record::run);
            if (owner != records_.end())
            {
                owner->views.push_back(*shown);
            }
        }
        return *shown;
    }
    EditorResult<void> ScenePlayback::Impl::step(RunId run)
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};

        if (closing_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "run.step.admission"});
        }
        auto owner = std::ranges::find(records_, std::optional{run}, &Record::run);
        const bool is_full = owner == records_.end() || owner->steps.size() >= 64;
        if (is_full)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.steps"});
        }
        auto step = runs_->step(run);
        if (!step)
        {
            return failure("run.step", step.error());
        }
        owner->steps.push_back(*step);
        return {};
    }

    ScenePlayback::ScenePlayback(
        std::shared_ptr<sessions::SessionStore> sessions,
        std::shared_ptr<RunStore> runs,
        std::shared_ptr<const ProjectionEnvironment> environment,
        lux::ui::Root& root,
        desktop::UiRegistry& windows,
        services::ServiceRegistry& services,
        services::ServiceScope& scope
    )
        : impl_(std::make_unique<Impl>(
              std::move(sessions), std::move(runs), std::move(environment), root, windows, services, scope
          ))
    {
    }
    ScenePlayback::~ScenePlayback() = default;
    EditorResult<void> ScenePlayback::Impl::requestClose() noexcept
    {
        if (auto checked = admission(); !checked)
        {
            return checked;
        }
        Dispatch dispatch{dispatching_};
        closing_ = true;
        for (auto& record : records_)
        {
            if (record.preparing)
            {
                record.preparing->cancel();
            }
        }
        return {};
    }
    bool ScenePlayback::Impl::settled() const noexcept
    {
        return std::ranges::none_of(records_, [](const auto& record) { return record.preparing || record.run; });
    }
    EditorResult<std::vector<RunPresentationInfo>> ScenePlayback::Impl::reports() const
    {
        if (auto checked = admission(); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        Dispatch dispatch{dispatching_};
        std::vector<RunPresentationInfo> result;
        result.reserve(records_.size());
        for (const auto& record : records_)
        {
            auto& info = result.emplace_back(RunPresentationInfo{
                record.start,
                record.source,
                record.run,
                record.views,
                {},
                record.failure,
                bool(record.preparing),
                bool(record.stopping)
            });
            info.steps.reserve(record.steps.size());
            for (const auto ticket : record.steps)
            {
                auto status = runs_->stepStatus(ticket);
                if (!status)
                {
                    return failure("run.result.step", status.error());
                }
                info.steps.push_back({ticket, *status});
            }
        }
        return result;
    }
    EditorResult<void> ScenePlayback::Impl::acknowledgeFailure(StartRunId id)
    {
        if (auto checked = admission(); !checked)
        {
            return checked;
        }
        Dispatch dispatch{dispatching_};
        auto record = std::ranges::find(records_, id, &Impl::Record::start);
        if (record == records_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "run.result"});
        }
        const bool owns_active_work = record->preparing || record->run;
        const bool cannot_acknowledge = !record->failure || owns_active_work;
        if (cannot_acknowledge)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.result"});
        }
        records_.erase(record);
        return {};
    }
    EditorResult<void> ScenePlayback::Impl::acknowledgeStep(StepTicket ticket)
    {
        if (auto checked = admission(); !checked)
        {
            return checked;
        }
        Dispatch dispatch{dispatching_};
        const auto acknowledged = runs_->acknowledgeStep(ticket);
        if (!acknowledged)
        {
            return failure("run.step.acknowledge", acknowledged.error());
        }
        for (auto& record : records_)
        {
            if (record.run == ticket.run)
            {
                std::erase(record.steps, ticket);
            }
        }
        return {};
    }
    EditorResult<void> ScenePlayback::Impl::forgetViews(std::span<const lux::ui::PaneHandle> ids) noexcept
    {
        if (auto checked = admission(); !checked)
        {
            return checked;
        }
        Dispatch dispatch{dispatching_};
        for (auto& record : records_)
        {
            std::erase_if(record.views, [&](auto id) { return std::ranges::find(ids, id) != ids.end(); });
        }
        return {};
    }
    EditorResult<void> ScenePlayback::Impl::requestStop(RunId id) noexcept
    {
        if (auto checked = admission(); !checked)
        {
            return checked;
        }
        Dispatch dispatch{dispatching_};
        auto record = std::ranges::find(records_, std::optional{id}, &Impl::Record::run);
        if (record == records_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "run.stop"});
        }
        record->stop_requested = true;
        return {};
    }
    EditorResult<StartRunId> ScenePlayback::play(sessions::ContentStamp content)
    {
        return impl_->play(content);
    }
    EditorResult<void> ScenePlayback::step(RunId id)
    {
        return impl_->step(id);
    }
    EditorResult<void> ScenePlayback::stop(RunId id)
    {
        return impl_->stop(id);
    }
    EditorResult<lux::ui::PaneHandle> ScenePlayback::showTool(lux::ui::PaneHandle source, ESceneTool kind)
    {
        return impl_->showTool(source, kind);
    }
    EditorResult<void> ScenePlayback::update(bool accept_prepared)
    {
        return impl_->update(accept_prepared);
    }
    EditorResult<void> ScenePlayback::requestClose() noexcept
    {
        return impl_->requestClose();
    }
    bool ScenePlayback::settled() const noexcept
    {
        return impl_->settled();
    }
    EditorResult<std::vector<RunPresentationInfo>> ScenePlayback::reports() const
    {
        return impl_->reports();
    }
    EditorResult<void> ScenePlayback::acknowledgeFailure(StartRunId id)
    {
        return impl_->acknowledgeFailure(id);
    }
    EditorResult<void> ScenePlayback::acknowledgeStep(StepTicket ticket)
    {
        return impl_->acknowledgeStep(ticket);
    }
    EditorResult<void> ScenePlayback::forgetViews(std::span<const lux::ui::PaneHandle> ids) noexcept
    {
        return impl_->forgetViews(ids);
    }
    EditorResult<void> ScenePlayback::requestStop(RunId id) noexcept
    {
        return impl_->requestStop(id);
    }
} // namespace lux::editor::scene

namespace lux::editor::scene
{
    namespace
    {
        constexpr services::ServiceContract playback_contracts[]{
            services::ServiceContract::forType<ScenePlayback, ScenePlayback>(
                services::ServiceNameView{"lux.editor.scene.playback"}
            )
        };
        constexpr services::ServiceDependency playback_dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.scene.runs"},
             1,
             cxx::typeToken<RunStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.scene.projection.environment"},
             1,
             cxx::typeToken<ProjectionEnvironment>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.ui.root"},
             1,
             cxx::typeToken<lux::ui::Root>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.ui"},
             1,
             cxx::typeToken<desktop::UiRegistry>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.registry"},
             1,
             cxx::typeToken<services::ServiceRegistry>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.scope"},
             1,
             cxx::typeToken<services::ServiceScope>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ScenePlayback>>
        createPlayback(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto sessions = resolver.get<sessions::SessionStore>(0);
            if (!sessions)
            {
                return cxx::unexpected(std::move(sessions.error()));
            }
            auto runs = resolver.get<RunStore>(1);
            if (!runs)
            {
                return cxx::unexpected(std::move(runs.error()));
            }
            auto environment = resolver.get<ProjectionEnvironment>(2);
            if (!environment)
            {
                return cxx::unexpected(std::move(environment.error()));
            }
            auto root = resolver.require<lux::ui::Root>(3);
            if (!root)
            {
                return cxx::unexpected(std::move(root.error()));
            }
            auto windows = resolver.require<desktop::UiRegistry>(4);
            if (!windows)
            {
                return cxx::unexpected(std::move(windows.error()));
            }
            auto services = resolver.require<services::ServiceRegistry>(5);
            if (!services)
            {
                return cxx::unexpected(std::move(services.error()));
            }
            auto scope = resolver.require<services::ServiceScope>(6);
            if (!scope)
            {
                return cxx::unexpected(std::move(scope.error()));
            }
            return std::make_unique<ScenePlayback>(
                std::move(*sessions),
                std::move(*runs),
                std::move(*environment),
                root->get(),
                windows->get(),
                services->get(),
                scope->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kScenePlaybackService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ScenePlayback, createPlayback>(
            services::ServiceNameView{"lux.editor.scene.playback"},
            playback_contracts,
            playback_dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* instance) noexcept -> services::ServiceResult<bool>
        { return static_cast<const ScenePlayback*>(instance)->settled(); };
        return descriptor;
    }();
} // namespace lux::editor::scene
