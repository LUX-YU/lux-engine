#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/detail/RenderAssets.hpp>

#include <algorithm>
#include <cmath>

namespace lux::scene
{
namespace
{
using Metadata = std::shared_ptr<const RenderSystemMetadata>;
constexpr std::array RenderRequirements{
    SceneSystemRequirementSpec{.name = "render_runtime",
                               .capability = "lux.render.runtime",
                               .expected_type = lux::cxx::typeToken<render::RenderRuntime>(),
                               .optional = false},
    SceneSystemRequirementSpec{.name = "render_metadata",
                               .capability = "lux.render.metadata",
                               .expected_type = lux::cxx::typeToken<Metadata>(),
                               .optional = false},
    SceneSystemRequirementSpec{.name = "render_assets",
                               .capability = "lux.render.assets",
                               .expected_type = lux::cxx::typeToken<std::shared_ptr<RenderAssetSource>>(),
                               .optional = true}};

SceneSystemBuildFailure failure(ESceneSystemBuildError code, system::SystemInstanceId system,
                                std::uint64_t subject = 0) noexcept
{
    return {code, system, {}, subject};
}
} // namespace

lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(SceneBuilder &builder,
                                                                             SceneSystemDescription description) noexcept
{
    auto *runtime = builder.require<render::RenderRuntime>(description.instanceId(), "render_runtime");
    auto *metadata = builder.require<Metadata>(description.instanceId(), "render_metadata");
    if (!runtime || !metadata || !*metadata)
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
    const auto &catalog = runtime->features();
    std::vector<std::string_view> roots;
    for (const auto &selected : config->features)
    {
        const auto name = catalog.nameOfType(selected.type);
        if (name.empty() || std::ranges::find(roots, name) != roots.end())
        {
            return lux::cxx::unexpected(
                failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), selected.type));
        }
        roots.push_back(name);
    }
    const auto order = catalog.resolveAttachOrder(roots);
    if (!order.unknown.empty() || !order.missing_deps.empty() || !order.cycle.empty())
    {
        return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()));
    }
    std::vector<render::SceneFeatureAttachment> features;
    std::vector<RenderSystem::Extraction> extractions;
    for (const auto name : order.order)
    {
        const auto *descriptor = catalog.descriptor(name);
        const auto *meta = descriptor ? (*metadata)->feature(descriptor->type) : nullptr;
        if (!meta || !meta->scene_configurable || !meta->registration || !meta->registration->configuration.valid())
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(),
                                                descriptor ? descriptor->type : 0));
        }
        const auto selected = std::ranges::find(config->features, meta->type, &RenderFeatureInstanceDescription::type);
        const std::span<const std::byte> portable = selected == config->features.end()
                                                        ? meta->default_configuration
                                                        : std::span<const std::byte>(selected->configuration);
        render::SceneFeatureAttachment attachment{meta->type, catalog.typeId(name), {}, *metadata};
        auto encoded = meta->registration->configuration.materialize_attach(portable, attachment.configuration);
        if (!encoded || !attachment.registered_type ||
            attachment.configuration.size() != meta->registration->configuration.attach_wire_size)
        {
            return lux::cxx::unexpected(
                failure(ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId(), meta->type));
        }
        features.push_back(std::move(attachment));
        if (meta->create_sync_stage)
        {
            extractions.push_back({meta->type, meta->create_sync_stage});
        }
    }
    const std::string name(description.instanceName());
    render::RenderControlSession::CreateSceneConfig create{};
    create.name = name.c_str();
    create.coordinate_page_size = config->coordinate_page_size;
    auto lease = runtime->createScene(create, features);
    if (!lease)
    {
        return lux::cxx::unexpected(SceneSystemBuildFailure{
            ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE, description.instanceId(), {}, 0, {}, lease.error()});
    }
    auto *assets = builder.require<std::shared_ptr<RenderAssetSource>>(description.instanceId(), "render_assets");
    auto *query = builder.findDependency<MeshQuerySystem>();
    auto system = builder.emplaceSystem<RenderSystem>(
        description.instanceId(), description.instanceId(), builder.sceneInstanceId(), *runtime, builder.registry(),
        std::move(*lease), config->coordinate_page_size, std::move(extractions),
        assets ? *assets : std::shared_ptr<RenderAssetSource>{}, query);
    if (!system)
    {
        return lux::cxx::unexpected(system.error());
    }
    auto maintenance = builder.addMaintenanceTask<RenderSystem>(
        description.instanceId(),
        [](RenderSystem &self, SceneStageContext &context) noexcept { return self.maintain(context); });
    if (!maintenance)
    {
        return maintenance;
    }
    return builder.addPublicationTask<RenderSystem>(
        description.instanceId(),
        [](RenderSystem &self, SceneStageContext &context) noexcept { return self.publish(context); });
}

RenderSystem::RenderSystem(system::SystemInstanceId id, SceneInstanceId scene, render::RenderRuntime &runtime,
                           simulation::ecs::Registry &registry, render::RenderSceneLease lease, double page_size,
                           std::vector<Extraction> factories, std::shared_ptr<RenderAssetSource> assets,
                           MeshQuerySystem *query, object::ObjectDispatcherRef dispatcher)
    : object::LuxObject(std::move(dispatcher)), instance_(id), view_associations_{scene}, runtime_(runtime),
      registry_(registry), coordinate_page_size_(page_size), lease_(std::move(lease)),
      assets_(std::make_unique<detail::RenderAssets>(registry, scene, lease_.receipt(), std::move(assets))),
      query_(query), factories_(std::move(factories))
{
    stages_.reserve(factories_.size());
}

RenderSystem::~RenderSystem() noexcept
{
    // SceneInstance has already revoked hook execution. Disconnect observers
    // while the Registry is still alive, then retire unaccepted input locally.
    stages_.clear();
    assets_.reset();
    statistics_.retired_unforwarded += prepared_ ? 1 : 0;
    pending_.clear_keep_capacity();
    // Lease destruction only marks its admitted release record. The runtime
    // continues accepted Programs, Views and GPU retirement independently.
}

render::RenderSceneId RenderSystem::renderSceneId() const noexcept
{
    return lease_.id();
}
render::SceneResourceStatus RenderSystem::resourceStatus() const noexcept
{
    return lease_.status();
}
render::RenderSceneReceipt RenderSystem::resourceReceipt() const noexcept
{
    return lease_.receipt();
}
render::FeatureHandle RenderSystem::feature(render::FeatureTypeId type) const noexcept
{
    return lease_.feature(type);
}
render::RenderResult<std::unique_ptr<render::RenderView>> RenderSystem::openView(render::ViewConfig config)
{
    return runtime_.openView(lease_, config);
}

render::RenderResult<void> RenderSystem::associateView(render::RenderViewId id, SceneInstanceId instance,
                                                       simulation::ecs::Entity entity)
{
    if (instance != view_associations_.instance || !registry_.valid(entity) || !registry_.all_of<Camera>(entity))
    {
        return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT, {}, id});
    }
    auto view = runtime_.observeView(id);
    if (!view)
    {
        return lux::cxx::unexpected(view.error());
    }
    if (view->scene != lease_.id() || view->status.state == render::EViewState::CLOSING ||
        view->status.state == render::EViewState::CLOSED)
    {
        return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::STALE_VIEW, {}, id});
    }
    if (view->status.failure)
    {
        return lux::cxx::unexpected(*view->status.failure);
    }
    const auto existing = std::ranges::find(view_bindings_, id, &ViewBinding::id);
    if (existing == view_bindings_.end())
    {
        view_bindings_.push_back({id, entity});
    }
    else
    {
        existing->camera = entity;
    }
    refreshViews();
    return {};
}

render::RenderResult<void> RenderSystem::dissociateView(render::RenderViewId id, SceneInstanceId instance,
                                                        simulation::ecs::Entity entity) noexcept
{
    const auto binding = std::ranges::find(view_bindings_, id, &ViewBinding::id);
    if (instance != view_associations_.instance || binding == view_bindings_.end() || binding->camera != entity)
    {
        return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::STALE_VIEW, {}, id});
    }
    view_bindings_.erase(binding);
    refreshViews();
    return {};
}

void RenderSystem::refreshViews()
{
    // Reuse storage; unchanged Main turns allocate nothing and do not dirty extraction.
    std::size_t count{};
    bool changed{};
    std::erase_if(view_bindings_, [&](const auto &binding) {
        auto observed = runtime_.observeView(binding.id);
        if (!registry_.valid(binding.camera) || !registry_.all_of<Camera>(binding.camera) || !observed ||
            observed->status.state == render::EViewState::CLOSING ||
            observed->status.state == render::EViewState::CLOSED)
        {
            return true;
        }
        if (observed->status.state != render::EViewState::READY)
        {
            return false;
        }
        const RenderViewAssociation value{observed->handle, binding.camera, observed->status.ready_extent};
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
        return false;
    });
    changed |= count != view_associations_.values.size();
    view_associations_.values.resize(count);
    view_associations_.revision += changed;
}

SceneStageResult RenderSystem::maintain(SceneStageContext &context) noexcept
{
    auto result = maintain();
    if (result && *result == ESceneProgress::COMPLETE)
    {
        assets_->maintain(context);
        if (query_)
        {
            assets_->synchronizeQuery(*query_);
        }
    }
    if (result && *result == ESceneProgress::COMPLETE)
    {
        context.invalidated |=
            full_sync_ || std::ranges::any_of(stages_, [](const auto &stage) { return stage->hasPendingChanges(); });
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
    const auto status = lease_.status();
    if (!status.failure.ok() || runtime.state != render::ERenderRuntimeState::ACTIVE)
    {
        const auto error = !status.failure.ok()  ? status.failure
                           : !runtime.error.ok() ? runtime.error
                                                 : render::renderError<render::err::comm::ChannelStopping>();
        result_ = lux::cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, error});
        return result_;
    }
    if (status.state != render::ESceneResourceState::READY)
    {
        return ESceneProgress::PENDING;
    }
    if (!initialized_)
    {
        // Construct into temporary owners; a failing factory does not leave
        // partly installed observers in the published extraction list.
        std::vector<std::unique_ptr<RenderSyncStage>> stages;
        stages.reserve(factories_.size());
        for (const auto &factory : factories_)
        {
            auto stage = factory.create({registry_,
                                         lease_.id(),
                                         runtime_.features(),
                                         factory.feature,
                                         lease_.feature(factory.feature),
                                         coordinate_page_size_,
                                         {},
                                         &view_associations_});
            if (!stage)
            {
                result_ = lux::cxx::unexpected(
                    SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, stage.error()});
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

SceneStageResult RenderSystem::submitProgram(render::RenderProgram<> &program, SceneStageContext &context) noexcept
{
    if (!context.publications)
    {
        return ESceneProgress::PENDING;
    }
    auto accepted = runtime_.submit(program);
    if (!accepted)
    {
        result_ = lux::cxx::unexpected(
            SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_, accepted.error()});
        return result_;
    }
    if (*accepted == render::EFrameSubmit::BACKPRESSURED)
    {
        ++statistics_.backpressured;
        return ESceneProgress::PENDING;
    }
    --context.publications;
    return ESceneProgress::COMPLETE;
}

SceneStageResult RenderSystem::publish(SceneStageContext &context) noexcept
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
        const bool changes =
            time_changed || std::ranges::any_of(stages_, [](const auto &stage) { return stage->hasPendingChanges(); });
        if (!changes)
        {
            return ESceneProgress::COMPLETE;
        }
        render::RenderProgramBuilder<> builder{pending_};
        builder.begin();
        pending_.kind = render::ERenderProgramKind::StateUpdate;
        bool commands = time_changed;
        if (time_changed)
        {
            builder.push(render::opcodes::CommandOp, render::type_ids::PublishSceneTime,
                         render::PublishSceneTimePayload{lease_.id(), context.elapsed.count(), context.delta.count(),
                                                         context.step});
        }
        for (auto &stage : stages_)
        {
            const auto prepared = stage->prepare(builder);
            if (prepared == ERenderSyncPrepareResult::FAILED || !builder.valid())
            {
                for (auto &value : stages_)
                {
                    value->discardPrepared();
                }
                pending_.clear_keep_capacity();
                result_ = lux::cxx::unexpected(
                    SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, instance_,
                                          RenderSyncFailure{ERenderSyncError::STAGE_PREPARE_FAILURE}});
                return result_;
            }
            commands = commands || prepared == ERenderSyncPrepareResult::PREPARED_COMMANDS;
        }
        // The owned packet captures this change set. New Registry changes
        // accumulate independently while this packet waits for admission.
        for (auto &stage : stages_)
        {
            stage->commitPrepared();
        }
        captured_elapsed_ = context.elapsed;
        captured_delta_ = context.delta;
        captured_step_ = context.step;
        if (!commands)
        {
            full_sync_ = false;
            return ESceneProgress::COMPLETE;
        }
        static_cast<void>(builder.emplaceAttachment<render::RenderSceneLease>(render::attachment_types::OwnedObject,
                                                                              lease_.retain()));
        prepared_ = true;
        ++statistics_.published;
        statistics_.pending = 1;
        statistics_.high_water = 1;
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
    statistics_.pending = 0;
    ++statistics_.forwarded;
    full_sync_ = false;
    return ESceneProgress::COMPLETE;
}

std::span<const RenderAssetStatus> RenderSystem::assetStatus() const
{
    return assets_->statuses();
}
std::uint64_t RenderSystem::assetRevision() const
{
    return assets_->revision();
}
render::RenderResult<void> RenderSystem::retryAsset(const RenderAssetKey &key)
{
    return assets_->retry(key);
}
render::RenderResult<void> RenderSystem::replaceAssetSource(std::shared_ptr<RenderAssetSource> source)
{
    if (!source || !source->uses(runtime_))
    {
        return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
    }
    assets_->replaceSource(std::move(source));
    return {};
}
RenderSyncStatistics RenderSystem::transportStatistics() const noexcept
{
    return statistics_;
}

SceneSystemRegistration builtinRenderSystemRegistration() noexcept
{
    return {.type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .configuration = lux::serialization::makePortableValueCodec<RenderSystemConfiguration>(),
            .observations = {},
            .requirements = RenderRequirements,
            .connections = {},
            .project_object = sceneSystemObjectProjection<RenderSystem>(),
            .install = &installBuiltinRenderSystem};
}
std::span<const SceneSystemRegistration> builtinRenderSystemRegistrations() noexcept
{
    static const std::array registrations{builtinRenderSystemRegistration()};
    return registrations;
}
} // namespace lux::scene
