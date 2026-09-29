#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>

#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/RenderAssets.hpp>

#include <algorithm>
#include <cmath>

namespace lux::scene
{
    namespace
    {

        constexpr std::array RenderRequirements{
            SceneSystemRequirementSpec{
                .name = "render_runtime",
                .capability = "lux.render.runtime",
                .expected_type = lux::cxx::typeToken<render::RenderRuntime>(),
                .optional = false
            },
            SceneSystemRequirementSpec{
                .name = "render_bindings",
                .capability = "lux.render.scene_bindings",
                .expected_type = lux::cxx::typeToken<RenderFeatureSceneBindings>(),
                .optional = false
            },
            SceneSystemRequirementSpec{
                .name = "render_resources",
                .capability = "lux.render.resources",
                .expected_type = lux::cxx::typeToken<RenderResources>(),
                .optional = false
            },
            SceneSystemRequirementSpec{
                .name = "render_assets",
                .capability = "lux.render.assets",
                .expected_type = lux::cxx::typeToken<RenderAssetInput>(),
                .optional = true
            }
        };

        SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system,
            std::uint64_t subject = 0
        ) noexcept
        {
            return {code, system, {}, subject};
        }
    } // namespace

    lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(
        SceneSystemInstaller& builder,
        SceneSystemDescription description
    ) noexcept
    {
        auto* runtime = builder.require<render::RenderRuntime>(description.instanceId(), "render_runtime");
        auto* bindings = builder.require<RenderFeatureSceneBindings>(description.instanceId(), "render_bindings");
        if (!runtime || !bindings)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::MISSING_REQUIREMENT, description.instanceId()));
        }
        auto config = builder.decodeConfiguration<RenderSystemConfiguration>(description);
        if (!config)
        {
            return lux::cxx::unexpected(config.error());
        }
        if (!std::isfinite(config->coordinate_page_size) || config->coordinate_page_size <= 0)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()));
        }
        const auto& catalog = runtime->features();
        std::vector<std::string_view> roots;
        for (const auto& selected : config->features)
        {
            const auto name = catalog.nameOfType(selected.type);
            if (name.empty() || std::ranges::find(roots, name) != roots.end())
            {
                return lux::cxx::unexpected(
                    failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), selected.type)
                );
            }
            roots.push_back(name);
        }
        const auto order = catalog.resolveAttachOrder(roots);
        if (!order.valid())
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()));
        }
        std::vector<SceneFeatureAttachment> features;
        std::vector<RenderSystem::Extraction> extractions;
        for (const auto name : order.order)
        {
            const auto* entry = catalog.find(name);
            const auto& registration = entry->registration;
            const auto type = registration.factory.descriptor.type;
            const auto& codec = registration.configuration;
            if (!registration.scene_configurable || !codec.valid())
                return lux::cxx::unexpected(
                    failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), type)
                );
            const auto selected = std::ranges::find(config->features, type, &RenderFeatureInstanceDescription::type);
            if (selected == config->features.end() || selected->configuration_schema != codec.schema ||
                selected->configuration_version != codec.schema_version)
                return lux::cxx::unexpected(
                    failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), type)
                );
            SceneFeatureAttachment attachment{type, entry->feature_type_id, {}, registration.code_lifetime};
            auto encoded = codec.materialize_attach(selected->configuration, attachment.configuration);
            if (!encoded)
                return lux::cxx::unexpected(
                    failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), type)
                );
            features.push_back(std::move(attachment));
            const auto binding = std::ranges::find_if(*bindings, [&](const auto& candidate) {
                return candidate.feature == type && candidate.scene_system == description.type();
            });
            if (binding != bindings->end() && binding->create_sync_stage)
                extractions.push_back({type, binding->create_sync_stage, binding->code_lifetime});
        }
        auto* resources = builder.require<RenderResources>(description.instanceId(), "render_resources");
        auto* assets = builder.require<RenderAssetInput>(description.instanceId(), "render_assets");
        const bool wrong_runtime = resources && !resources->uses(*runtime);
        const bool missing_resources = !resources;
        const bool invalid_input = assets && !*assets;
        if (wrong_runtime || missing_resources || invalid_input)
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()));
        const std::string name(description.instanceName());
        render::RenderControlSession::CreateSceneConfig create{};
        create.name = name.c_str();
        create.coordinate_page_size = config->coordinate_page_size;
        auto resource = resources->requestScene(create, std::move(features));
        if (!resource)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{
                ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                description.instanceId(),
                {},
                0,
                {},
                resource.error()
            });
        }
        auto* query = builder.findDependency<MeshQuerySystem>() ? builder.registry().ctx().find<MeshQuery>() : nullptr;
        auto system = builder.emplaceSystem<RenderSystem>(
            description.instanceId(),
            description.instanceId(),
            builder.sceneInstanceId(),
            *runtime,
            builder.registry(),
            *resource,
            config->coordinate_page_size,
            std::move(extractions),
            *resources,
            assets ? *assets : RenderAssetInput{},
            query
        );
        resources->release(*resource);
        if (!system)
        {
            return lux::cxx::unexpected(system.error());
        }
        auto maintenance = builder.addMaintenanceTask<RenderSystem>(
            description.instanceId(),
            [](RenderSystem& self, SceneStageContext& context) noexcept { return self.maintain(context); }
        );
        if (!maintenance)
        {
            return maintenance;
        }
        return builder.addPublicationTask<RenderSystem>(
            description.instanceId(),
            [](RenderSystem& self, SceneStageContext& context) noexcept { return self.publish(context); }
        );
    }

    RenderSystem::RenderSystem(
        system::SystemInstanceId id,
        SceneInstanceId scene,
        render::RenderRuntime& runtime,
        simulation::ecs::Registry& registry,
        RenderResourceId resource,
        double page_size,
        std::vector<Extraction> factories,
        RenderResources& resources,
        RenderAssetInput assets,
        MeshQuery* query
    )
        : instance_(id), view_associations_{scene}, runtime_(runtime), registry_(registry),
          scene_state_{id, resource, page_size}, resources_(resources), receipt_(resources.sceneReceipt(resource)),
          assets_(RenderAssets::install(registry, id, scene, resources, std::move(assets))), query_(query),
          factories_(std::move(factories))
    {
        registry_.ctx().emplace<RenderSceneState::Entry>(std::cref(scene_state_));
        stages_.reserve(factories_.size());
        request_created_ = registry_.on_construct<RenderViewRequest>().connect<&RenderSystem::requestChanged>(*this);
        request_updated_ = registry_.on_update<RenderViewRequest>().connect<&RenderSystem::requestChanged>(*this);
        request_destroyed_ = registry_.on_destroy<RenderViewRequest>().connect<&RenderSystem::requestDestroyed>(*this);
        for (auto entity : registry_.view<RenderViewRequest>())
            requestChanged(registry_, entity);
        if (!resources_.retain(scene_state_.resource))
            render::renderFatal("Invalid RenderSystem Scene resource");
    }

    RenderSystem::~RenderSystem() noexcept
    {
        beginRetirement();
    }

    void RenderSystem::beginRetirement() noexcept
    {
        if (std::exchange(retiring_, true))
            return;
        // SceneInstance has already revoked hook execution. Disconnect observers
        // while the Registry is still alive, then retire unaccepted input locally.
        request_created_.release();
        request_updated_.release();
        request_destroyed_.release();
        for (auto& request : view_requests_)
            request.cancellation.reset();
        stages_.clear();
        registry_.ctx().erase<RenderSceneState::Entry>();
        RenderAssets::uninstall(registry_, instance_);
        scene_state_.transport.retired_unforwarded += prepared_ ? 1 : 0;
        pending_.clear_keep_capacity();
        for (const auto& request : view_requests_)
            if (request.view.isValid())
                resources_.release(request.view);
        for (const auto view : retiring_views_)
            resources_.release(view);
        resources_.release(scene_state_.resource);
    }

    render::RenderSceneId RenderSystem::renderSceneId() const noexcept
    {
        return resourceStatus().scene;
    }
    SceneResourceStatus RenderSystem::resourceStatus() const noexcept
    {
        return receipt_.status();
    }
    RenderSceneReceipt RenderSystem::resourceReceipt() const noexcept
    {
        return receipt_;
    }
    render::RenderResult<render::RenderSubmissionState> RenderSystem::captureScene() const noexcept
    {
        return resources_.capture(std::span{&scene_state_.resource, 1});
    }
    render::FeatureHandle RenderSystem::feature(render::FeatureTypeId type) const noexcept
    {
        return resources_.sceneFeature(scene_state_.resource, type);
    }
    void RenderSystem::refreshViews()
    {
        std::size_t count{};
        bool changed{};
        const auto append = [&](RenderResourceId id,
                                simulation::ecs::Entity camera,
                                std::uint64_t revision,
                                simulation::ecs::Entity request) {
            auto observed = resources_.observeView(id);
            if (!observed)
                return;
            const bool is_closing =
                observed->status.state == EViewState::CLOSING || observed->status.state == EViewState::CLOSED;
            const bool has_extent = observed->render_extent.width && observed->render_extent.height;
            if (is_closing || observed->handle.isNull() || !has_extent)
                return;
            const RenderViewAssociation value{
                observed->handle,
                camera,
                observed->render_extent,
                id,
                instance_,
                revision,
                observed->render_sequence,
                request
            };
            if (count == view_associations_.values.size())
            {
                view_associations_.values.push_back(value);
                changed = true;
            }
            else if (view_associations_.values[count] != value)
            {
                view_associations_.values[count] = value;
                changed = true;
            }
            ++count;
        };
        for (const auto& request : view_requests_)
        {
            const auto* result = registry_.try_get<RenderViewResult>(request.entity);
            if (request.view.isValid() && result && !result->failure && !request.removed)
                append(request.view, request.adopted.camera, request.adopted.revision, request.entity);
        }
        changed |= count != view_associations_.values.size();
        view_associations_.values.resize(count);
        view_associations_.revision += changed;
    }

    SceneStageResult RenderSystem::maintain(SceneStageContext& context) noexcept
    {
        if (context.stop.stop_requested())
        {
            beginRetirement();
            return receipt_.status().state == ESceneResourceState::RETIRED ? ESceneProgress::COMPLETE
                                                                           : ESceneProgress::PENDING;
        }
        auto result = maintain();
        if (result && *result == ESceneProgress::COMPLETE)
        {
            if (context.allow_structure)
            {
                maintainViewRequests();
                refreshViews();
            }
            assets_.maintain(context);
            if (query_)
            {
                assets_.synchronizeQuery(*query_);
            }
        }
        if (result && *result == ESceneProgress::COMPLETE)
        {
            if (context.allow_structure)
                context.publication_needed |=
                    full_sync_ || !retiring_views_.empty() || forwarded_views_ != view_associations_.revision ||
                    std::ranges::any_of(stages_, [](const auto& stage) { return stage->hasPendingChanges(); });
        }
        return result;
    }

    SceneStageResult RenderSystem::maintain() noexcept
    {
        refreshViews();
        if (!result_)
        {
            return result_;
        }
        const auto runtime = runtime_.status();
        const auto status = resourceStatus();
        if (!status.failure.ok() || runtime.state != render::ERenderRuntimeState::ACTIVE)
        {
            const auto error = !status.failure.ok()  ? status.failure
                               : !runtime.error.ok() ? runtime.error
                                                     : render::renderError<render::err::comm::ChannelStopping>();
            result_ =
                lux::cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, error});
            return result_;
        }
        if (status.state != ESceneResourceState::READY)
        {
            return ESceneProgress::PENDING;
        }
        if (!initialized_)
        {
            // Construct into temporary owners; a failing factory does not leave
            // partly installed observers in the published extraction list.
            std::vector<std::unique_ptr<RenderSyncStage>> stages;
            stages.reserve(factories_.size());
            for (const auto& factory : factories_)
            {
                auto stage = factory.create(
                    {registry_,
                     renderSceneId(),
                     runtime_.features(),
                     factory.feature,
                     feature(factory.feature),
                     scene_state_.coordinate_page_size,
                     {},
                     &view_associations_}
                );
                if (!stage)
                {
                    result_ = lux::cxx::unexpected(
                        SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, stage.error()}
                    );
                    return result_;
                }
                (*stage)->requestFullSync();
                stages.push_back(std::move(*stage));
            }
            stages_ = std::move(stages);
            initialized_ = true;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::submitProgram(render::TRenderProgram<>& program, SceneStageContext& context) noexcept
    {
        auto accepted = runtime_.submit(program);
        if (!accepted)
        {
            result_ = lux::cxx::unexpected(
                SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, accepted.error()}
            );
            return result_;
        }
        if (*accepted == render::EFrameSubmit::BACKPRESSURED)
        {
            ++scene_state_.transport.backpressured;
            return ESceneProgress::PENDING;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::publish(SceneStageContext& context) noexcept
    {
        const auto ready = maintain();
        if (!ready || *ready == ESceneProgress::PENDING)
        {
            return ready;
        }
        if (!prepared_)
        {
            const bool time_changed = full_sync_ || captured_elapsed_ != context.elapsed ||
                                      captured_delta_ != context.delta || captured_step_ != context.step;
            const bool views_changed = captured_views_ != view_associations_.revision || !retiring_views_.empty();
            const bool changes = time_changed || views_changed || std::ranges::any_of(stages_, [](const auto& stage) {
                                     return stage->hasPendingChanges();
                                 });
            if (!changes)
            {
                return ESceneProgress::COMPLETE;
            }
            render::RenderProgramBuilder<> builder{pending_};
            builder.begin();
            pending_.kind = render::ERenderProgramKind::STATE_UPDATE;
            bool commands = time_changed || views_changed;
            if (commands)
            {
                builder.push(
                    render::opcodes::CommandOp,
                    render::type_ids::PublishSceneTime,
                    render::PublishSceneTimePayload{
                        renderSceneId(),
                        context.elapsed.count(),
                        context.delta.count(),
                        context.step
                    }
                );
            }
            for (auto& stage : stages_)
            {
                const auto prepared = stage->prepare(builder);
                if (prepared == ERenderSyncPrepareResult::FAILED || !builder.valid())
                {
                    for (auto& value : stages_)
                    {
                        value->discardPrepared();
                    }
                    pending_.clear_keep_capacity();
                    result_ = lux::cxx::unexpected(SceneExecutionFailure{
                        ESceneExecutionError::SYSTEM_FAILURE,
                        instance_,
                        RenderSyncFailure{ERenderSyncError::STAGE_PREPARE_FAILURE}
                    });
                    return result_;
                }
                const bool has_frame = prepared == ERenderSyncPrepareResult::PREPARED_FRAME_COMMANDS;
                commands = commands || has_frame || prepared == ERenderSyncPrepareResult::PREPARED_COMMANDS;
                if (has_frame)
                    pending_.kind = render::ERenderProgramKind::FRAME;
            }
            if (commands)
            {
                publication_resources_.clear();
                publication_resources_.push_back(scene_state_.resource);
                for (const auto& view : view_associations_.values)
                    publication_resources_.push_back(view.resource);
                publication_resources_
                    .insert(publication_resources_.end(), retiring_views_.begin(), retiring_views_.end());
                auto use = resources_.capture(publication_resources_);
                if (!use)
                {
                    for (auto& stage : stages_)
                        stage->discardPrepared();
                    pending_.clear_keep_capacity();
                    result_ = lux::cxx::unexpected(
                        SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, use.error()}
                    );
                    return result_;
                }
                builder.emplaceAttachment<render::RenderSubmissionState>(
                    render::attachment_types::SubmissionState,
                    std::move(*use)
                );
            }
            // The owned packet captures this change set. New Registry changes
            // accumulate independently while this packet waits for admission.
            for (auto& stage : stages_)
            {
                stage->commitPrepared();
            }
            for (const auto view : retiring_views_)
                resources_.release(view);
            retiring_views_.clear();
            captured_elapsed_ = context.elapsed;
            captured_delta_ = context.delta;
            captured_step_ = context.step;
            captured_views_ = view_associations_.revision;
            captureViewPublications();
            if (!commands)
            {
                full_sync_ = false;
                return ESceneProgress::COMPLETE;
            }
            prepared_ = true;
            ++scene_state_.transport.published;
            scene_state_.transport.pending = 1;
            scene_state_.transport.high_water = 1;
        }
        auto accepted = submitProgram(pending_, context);
        if (!accepted)
        {
            return accepted;
        }
        if (*accepted == ESceneProgress::PENDING)
        {
            return accepted;
        }
        prepared_ = false;
        scene_state_.transport.pending = 0;
        ++scene_state_.transport.forwarded;
        forwarded_views_ = captured_views_;
        commitViewPublications();
        full_sync_ = false;
        return ESceneProgress::COMPLETE;
    }

    RenderSyncStatistics RenderSystem::transportStatistics() const noexcept
    {
        return scene_state_.transport;
    }

    SceneSystemRegistration builtinRenderSystemRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .configuration = renderSystemConfigurationCodec(),
            .observations = {},
            .requirements = RenderRequirements,

            .install = &installBuiltinRenderSystem
        };
    }
    std::span<const SceneSystemRegistration> builtinRenderSystemRegistrations() noexcept
    {
        static const std::array registrations{builtinRenderSystemRegistration()};
        return registrations;
    }
} // namespace lux::scene
