#include <algorithm>
#include <lux/engine/editor/scene/ModelPlacementService.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

namespace lux::editor::scene
{
    namespace
    {
        bool retryable(const ModelCreationFailure& failure) noexcept
        {
            return std::visit(
                [](const auto& error)
                {
                    using Error = std::decay_t<decltype(error)>;
                    if constexpr (std::same_as<Error, SceneEditError>)
                    {
                        const bool is_busy = error.code == ESceneEditError::BUSY;
                        const bool is_session_busy =
                            error.code == ESceneEditError::SESSION && error.session == sessions::ESessionError::BUSY;
                        return is_busy || is_session_busy;
                    }
                    else if constexpr (std::same_as<Error, project::VProjectQueryFailure>)
                    {
                        const auto* query = std::get_if<project::EProjectQueryError>(&error);
                        return query && *query == project::EProjectQueryError::BUSY;
                    }
                    else
                    {
                        return false;
                    }
                },
                failure.cause
            );
        }
        auto rejected(EEditorError error)
        {
            return cxx::unexpected(EditorFailure{error, "scene.model-placement"});
        }
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
        };
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ModelPlacementService, ModelPlacementService>(
                services::ServiceNameView{"lux.editor.scene.model-placement"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.simulation.components"},
             1,
             cxx::typeToken<simulation::ecs::ComponentSchemaSet>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ModelPlacementService>>
        createPlacement(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto execution = resolver.require<process::ExecutionRuntime>(0);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            auto sessions = resolver.require<sessions::SessionStore>(1);
            if (!sessions)
            {
                return cxx::unexpected(std::move(sessions.error()));
            }
            auto project = resolver.require<ProjectStorage>(2);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
            }
            auto schemas = resolver.require<simulation::ecs::ComponentSchemaSet>(3);
            if (!schemas)
            {
                return cxx::unexpected(std::move(schemas.error()));
            }
            return std::make_unique<ModelPlacementService>(
                execution->get(),
                sessions->get().access<SceneSession>(),
                project->get(),
                schemas->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kModelPlacementService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ModelPlacementService, createPlacement>(
            services::ServiceNameView{"lux.editor.scene.model-placement"},
            contracts,
            dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* allocation) noexcept -> services::ServiceResult<bool>
        { return static_cast<const ModelPlacementService*>(allocation)->settled(); };
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            auto result = static_cast<ModelPlacementService*>(allocation)->update();
            if (!result)
            {
                const auto& error = result.error();
                return cxx::unexpected(services::ServiceFailure{
                    error.code == EEditorError::BUSY ? services::EServiceError::BUSY
                                                     : services::EServiceError::FACTORY_FAILURE,
                    error.message,
                    error.domain,
                    static_cast<std::uint64_t>(error.code)
                });
            }
            return {};
        };
        return descriptor;
    }();
    ModelPlacementReport::ModelPlacementReport(std::uint64_t value, ModelPlacement input)
        : id(value), placement(std::move(input))
    {
    }
    ModelPlacementReport::~ModelPlacementReport() = default;
    ModelPlacementReport::ModelPlacementReport(ModelPlacementReport&&) noexcept = default;
    ModelPlacementReport& ModelPlacementReport::operator=(ModelPlacementReport&&) noexcept = default;
    bool ModelPlacementReport::active() const noexcept
    {
        return bool(operation_);
    }

    struct ModelPlacementService::Impl final
    {
        const std::thread::id owner_{std::this_thread::get_id()};
        process::ExecutionRuntime& execution_;
        sessions::TSessionAccess<SceneSession> sessions_;
        ProjectStorage& project_;
        simulation::ecs::ComponentSchemaSet schemas_;
        std::size_t capacity_;
        std::vector<ModelPlacementReport> reports_;
        std::uint64_t next_{1};
        bool dispatching_{}, closing_{};
        Impl(
            process::ExecutionRuntime& execution,
            sessions::TSessionAccess<SceneSession> sessions,
            ProjectStorage& project,
            simulation::ecs::ComponentSchemaSet schemas,
            std::size_t capacity
        )
            : execution_(execution), sessions_(sessions), project_(project), schemas_(std::move(schemas)),
              capacity_(capacity)
        {
            reports_.reserve(capacity_);
        }
        EditorResult<void> admission() const noexcept
        {
            if (owner_ != std::this_thread::get_id())
            {
                return rejected(EEditorError::INVALID_STATE);
            }
            if (dispatching_)
            {
                return rejected(EEditorError::BUSY);
            }
            return {};
        }
        void advance(ModelPlacementReport& entry)
        {
            if (entry.result || entry.failure)
            {
                return;
            }
            if (!entry.operation_)
            {
                if (entry.cancel_requested)
                {
                    entry.result.emplace(cxx::unexpected(ModelCreationFailure{process::TaskCancelled{}}));
                    return;
                }
                auto reads = project_.captureAssetReads();
                if (!reads)
                {
                    if (reads.error().code != EEditorError::BUSY)
                    {
                        entry.failure = reads.error();
                    }
                    return;
                }
                auto started = ModelCreationOperation::start(
                    execution_,
                    sessions_,
                    project_.catalogModel(),
                    std::move(*reads),
                    schemas_,
                    entry.placement
                );
                if (!started)
                {
                    if (!retryable(started.error()))
                    {
                        entry.result.emplace(cxx::unexpected(std::move(started.error())));
                    }
                    return;
                }
                entry.operation_ = std::move(*started);
            }
            if (entry.cancel_requested)
            {
                entry.operation_->cancel();
            }
            if (!entry.operation_->settled())
            {
                return;
            }
            auto result = entry.operation_->commit();
            if (!result && retryable(result.error()))
            {
                return;
            }
            entry.result.emplace(std::move(result));
            // Completion has returned; cleanup stays inside dispatch, including foreign payload destructors.
            entry.operation_.reset();
        }
    };
    ModelPlacementService::ModelPlacementService(
        process::ExecutionRuntime& execution,
        sessions::TSessionAccess<SceneSession> sessions,
        ProjectStorage& project,
        simulation::ecs::ComponentSchemaSet schemas,
        std::size_t capacity
    )
        : impl_(std::make_unique<Impl>(execution, sessions, project, std::move(schemas), capacity))
    {
    }
    ModelPlacementService::~ModelPlacementService() = default;
    EditorResult<std::uint64_t> ModelPlacementService::request(ModelPlacement input)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Dispatch dispatch{impl_->dispatching_};
        if (impl_->closing_)
        {
            return rejected(EEditorError::CLOSING);
        }
        const bool is_full = impl_->reports_.size() == impl_->capacity_;
        const bool is_exhausted = impl_->next_ == UINT64_MAX;
        if (is_full || is_exhausted)
        {
            return rejected(EEditorError::CAPACITY);
        }
        const auto id = impl_->next_++;
        impl_->reports_.emplace_back(id, std::move(input));
        return id;
    }
    EditorResult<void> ModelPlacementService::update() noexcept
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        for (auto& entry : impl_->reports_)
        {
            impl_->advance(entry);
        }
        return {};
    }
    EditorResult<void> ModelPlacementService::cancel(std::uint64_t id) noexcept
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        const auto found = std::ranges::find(impl_->reports_, id, &ModelPlacementReport::id);
        if (found == impl_->reports_.end())
        {
            return rejected(EEditorError::STALE_REQUEST);
        }
        found->cancel_requested = true;
        return {};
    }
    EditorResult<void> ModelPlacementService::acknowledge(std::uint64_t id) noexcept
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        const auto found = std::ranges::find(impl_->reports_, id, &ModelPlacementReport::id);
        if (found == impl_->reports_.end())
        {
            return rejected(EEditorError::STALE_REQUEST);
        }
        const bool has_result = found->result.has_value() || found->failure.has_value();
        if (!has_result || found->active())
        {
            return rejected(EEditorError::BUSY);
        }
        // Erase before the last result/payload code can call back, while the mutation guard remains active.
        auto retired = std::move(*found);
        impl_->reports_.erase(found);
        return {};
    }
    EditorResult<void> ModelPlacementService::requestClose() noexcept
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        Dispatch dispatch{impl_->dispatching_};
        impl_->closing_ = true;
        for (auto& entry : impl_->reports_)
        {
            entry.cancel_requested = true;
        }
        return {};
    }
    bool ModelPlacementService::settled() const noexcept
    {
        return std::ranges::all_of(
            impl_->reports_,
            [](const auto& entry) { return !entry.active() && (entry.result.has_value() || entry.failure.has_value()); }
        );
    }
    std::span<const ModelPlacementReport> ModelPlacementService::reports() const noexcept
    {
        return impl_->reports_;
    }
} // namespace lux::editor::scene
