#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>

#include <lux/engine/runtime/assets/SceneAssetServices.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        void addRequiredExtension(
            std::vector<lux::scene::RequiredExtension>& requirements,
            const lux::extensions::ExtensionId& provider,
            const lux::extensions::ExtensionModuleManager* modules)
        {
            if (!provider.isValid() || provider.name() == "org.lux.builtin" ||
                std::ranges::any_of(
                    requirements,
                    [&provider](const auto& value)
                    {
                        return value.id.name() == provider.name();
                    }) ||
                !modules)
            {
                return;
            }
            const auto module = modules->find(provider.view());
            if (!module)
                return;
            requirements.push_back({
                provider,
                module->version().major,
                module->version().minor});
        }

        [[nodiscard]] lux::cxx::expected<
            lux::scene::SceneDescription,
            std::string>
        openSceneDescription(
            const SceneRuntime::Config& config,
            const lux::asset::AssetManager& assets) noexcept
        {
            if (config.scene_asset_id.is_nil())
            {
                return lux::cxx::unexpected(std::string{
                    "SceneRuntime requires a non-nil SceneAsset id"});
            }
            const auto* scene = assets.fetchAssetAs<lux::scene::SceneAsset>(
                config.scene_asset_id);
            if (scene == nullptr || !scene->hasData())
            {
                return lux::cxx::unexpected(std::string{
                    "SceneAsset is absent or has no decoded data"});
            }
            if (scene->info() == nullptr ||
                scene->info()->id != scene->data()->id)
            {
                return lux::cxx::unexpected(std::string{
                    "SceneAsset shell and SceneDescription identity differ"});
            }
            const auto valid = lux::scene::validateSceneDescription(
                *scene->data());
            if (!valid)
                return lux::cxx::unexpected(valid.error().detail);
            return *scene->data();
        }
    }

    SceneRuntimePersistenceSnapshot SceneRuntime::persistenceSnapshot(
        std::span<const std::string> persistent_component_schemas) const
    {
        SceneRuntimePersistenceSnapshot result;
        if (scene_contribution_host_)
        {
            const auto activations =
                scene_contribution_host_->activationSnapshot();
            std::vector<lux::scene::SceneFeatureId> provider_closure;
            const auto addProviderClosure = [&](
                auto&& self,
                lux::scene::SceneFeatureIdView feature) -> void
            {
                if (!scene_contribution_catalog_ ||
                    std::ranges::any_of(
                        provider_closure,
                        [feature](const auto& visited)
                        {
                            return lux::scene::sameSceneFeatureId(
                                visited.view(), feature);
                        }))
                    return;
                const auto* descriptor =
                    scene_contribution_catalog_->find(feature);
                if (!descriptor)
                    return;
                provider_closure.push_back(descriptor->id);
                addRequiredExtension(
                    result.required_extensions,
                    descriptor->provider,
                    extension_modules_);
                for (const auto& dependency :
                     descriptor->required_contributions)
                    self(self, dependency.view());
            };

            for (const auto& activation : activations)
            {
                if (!activation.root || activation.persistence ==
                        EActivationPersistence::TRANSIENT)
                    continue;
                result.features.push_back({
                    lux::scene::SceneFeatureId{
                        activation.feature.name()},
                    activation.config.schema_version,
                    std::vector<std::byte>{
                        activation.config.bytes.view().begin(),
                        activation.config.bytes.view().end()}});
                addProviderClosure(
                    addProviderClosure,
                    activation.feature.view());
            }
        }

        for (const auto& schema_name : persistent_component_schemas)
        {
            const auto* component = components_.findBySchema(schema_name);
            if (!component)
                continue;
            addRequiredExtension(
                result.required_extensions,
                lux::extensions::ExtensionId{component->provider},
                extension_modules_);
        }

        std::ranges::sort(
            result.features,
            {},
            [](const auto& value) { return value.id.name(); });
        std::ranges::sort(
            result.required_extensions,
            {},
            [](const auto& value) { return value.id.name(); });
        return result;
    }

    SceneRuntime::SceneRuntime(
        const Dependencies& deps,
        std::unique_ptr<ISceneRuntimeIntegration> integration) noexcept
        : owner_thread_(std::this_thread::get_id())
        , assets_(deps.assets)
        , asset_client_(deps.asset_client)
        , async_(deps.async)
        , components_(deps.components)
        , entity_section_loading_(deps.entity_sections)
        , scene_contribution_catalog_(deps.scene_contribution_catalog)
        , extension_modules_(deps.extension_modules)
        , integration_(std::move(integration))
        , async_scope_(std::make_unique<lux::exec::AsyncScope>(deps.async))
    {}

    std::unique_ptr<SceneRuntime> SceneRuntime::create(
        const Dependencies& deps,
        const Config& config,
        std::unique_ptr<ISceneRuntimeIntegration> integration)
    {
        std::unique_ptr<SceneRuntime> runtime(
            new SceneRuntime(deps, std::move(integration)));
        if (!runtime->bringUp(config))
            return nullptr;
        return runtime;
    }

    SceneRuntime::~SceneRuntime()
    {
        const auto report = advanceClose();
        requireTerminalCloseBeforeDestruction(report, "destructor");
    }

    void SceneRuntime::requireTerminalCloseBeforeDestruction(
        SceneCloseReport report,
        std::string_view context) noexcept
    {
        if (report.terminal())
            return;
        lux::log::error(
            "scene",
            "SceneRuntime {} cannot release an owner while close callbacks "
            "still borrow it (error={}, integration_closed={})",
            context,
            toString(report.error),
            report.integration_closed);
        std::abort();
    }

    bool SceneRuntime::bringUp(const Config& config)
    {
        if (live_)
            return true;
        events_ = config.events;

        auto package_result = openSceneDescription(config, assets_);
        if (!package_result)
        {
            lux::log::error(
                "scene",
                "bringUp: failed to open SceneDescription '{}': {}",
                config.scene_origin,
                package_result.error());
            return failBringUp();
        }
        auto catalog_result = entity_scene::EntitySceneCatalog::create(
            std::move(*package_result));
        if (!catalog_result)
        {
            lux::log::error(
                "scene",
                "bringUp: SceneDescription catalog rejected '{}': {}",
                config.scene_origin,
                catalog_result.error().detail);
            return failBringUp();
        }
        auto catalog_owner = std::make_unique<
            entity_scene::EntitySceneCatalog>(
                std::move(*catalog_result));
        auto* const catalog = catalog_owner.get();
        const auto& package = catalog->package();
        scene_asset_ref_ = assets_.acquire(config.scene_asset_id);
        if (!scene_asset_ref_)
        {
            lux::log::error(
                "scene",
                "bringUp: failed to retain SceneAsset '{}'",
                uuids::to_string(config.scene_asset_id));
            return failBringUp();
        }

        for (const auto& requirement : package.required_extensions)
        {
            if (!extension_modules_)
            {
                lux::log::error(
                    "scene",
                    "bringUp: required extension '{}' is unavailable",
                    requirement.id.name());
                return failBringUp();
            }
            const auto status = extension_modules_->requirementStatus(
                requirement.id.view(),
                requirement.required_major,
                requirement.minimum_minor);
            if (status !=
                lux::extensions::EExtensionRequirementStatus::READY)
            {
                lux::log::error(
                    "scene",
                    "bringUp: required extension '{}' is not ready "
                    "(status={}, required={}.{})",
                    requirement.id.name(),
                    static_cast<unsigned>(status),
                    requirement.required_major,
                    requirement.minimum_minor);
                return failBringUp();
            }
        }

        auto candidate_world = std::make_unique<lux::ecs::World>();
        auto candidate_persistent_entities =
            std::make_unique<lux::ecs::PersistentEntityIndex>(
                candidate_world->registry());
        auto candidate_services =
            std::make_unique<lux::ecs::SceneServices>();
        auto candidate_schedule =
            std::make_unique<lux::ecs::Schedule>(*candidate_world);
        (void)lux::ecs::ensureHierarchyIndex(candidate_world->registry());
        lux::ecs::ScheduleBuilder builder{
            *candidate_schedule,
            *candidate_services};

        if (!builder.services().emplace<SceneAsyncContext>(
                async_, *async_scope_))
        {
            lux::log::error(
                "scene",
                "failed to publish scene asynchronous work context");
            return failBringUp();
        }

        if (!builder.services().emplace(std::move(catalog_owner)))
        {
            lux::log::error(
                "scene",
                "failed to publish immutable SceneDescription catalog service");
            return failBringUp();
        }

        if (!builder.services().adopt(*candidate_persistent_entities))
        {
            lux::log::error(
                "scene",
                "failed to publish persistent entity index service");
            return failBringUp();
        }
        if (!builder.services().emplace<
                lux::asset_runtime::SceneAssetServices>(
                    assets_,
                    asset_client_))
        {
            lux::log::error(
                "scene",
                "failed to publish scene asset services");
            return failBringUp();
        }

        auto loader = std::make_unique<
            entity_scene::EntitySectionLoaderSystem>(
                async_,
                entity_section_loading_,
                config.section_vfs ? config.section_vfs : assets_.vfs(),
                components_,
                *candidate_persistent_entities);
        auto* const loader_owner = loader.get();
        if (!builder.services().emplace<entity_scene::EntitySectionClient>(
                loader_owner->client()) ||
            !builder.services().emplace<entity_scene::ContentBlobClient>(
                loader_owner->contentBlobs()))
        {
            lux::log::error(
                "scene",
                "failed to publish EntitySection access services");
            return failBringUp();
        }
        if (!builder.add(
                std::move(loader),
                lux::ecs::kPhaseSceneLoading))
        {
            lux::log::error(
                "scene",
                "EntitySection loader assembly failed");
            return failBringUp();
        }

        SceneRuntimeAssemblyContext assembly_context{
            builder,
            assets_,
            config.name};
        if (integration_ && !integration_->prepare(assembly_context))
        {
            lux::log::error(
                "scene",
                "scene integration rejected assembly preparation");
            return failBringUp();
        }

        std::optional<SceneContributionBootstrap> contribution_bootstrap;
        if (!package.features.empty())
        {
            if (!scene_contribution_catalog_)
            {
                lux::log::error(
                    "scene",
                    "SceneDescription features require a catalog");
                return failBringUp();
            }
            std::vector<SceneContributionSelection> selected;
            selected.reserve(package.features.size());
            for (const auto& feature : package.features)
            {
                selected.push_back(SceneContributionSelection{
                    lux::scene::SceneFeatureId{feature.id.name()},
                    ContributionConfig{
                        feature.config_schema_version,
                        lux::cxx::SharedBytes<>::copyOf(
                            feature.config)}});
            }
            auto assembled = scene_contribution_catalog_->stageBootstrap(
                builder,
                selected);
            if (!assembled)
            {
                lux::log::error(
                    "scene",
                    "scene feature assembly failed for '{}' "
                    "(status={})",
                    assembled.error().feature.name(),
                    static_cast<unsigned>(assembled.error().code));
                return failBringUp();
            }
            contribution_bootstrap.emplace(std::move(*assembled));
        }

        if (integration_ && !integration_->finalize(assembly_context))
        {
            lux::log::error(
                "scene",
                "scene integration rejected assembly finalization");
            return failBringUp();
        }

        auto startup_result = entity_scene::StartupSectionSystem::create(
            *catalog,
            *loader_owner,
            async_);
        if (!startup_result)
        {
            lux::log::error(
                "scene",
                "startup EntitySection selection failed: {}",
                startup_result.error().detail);
            return failBringUp();
        }
        auto startup = std::move(*startup_result);
        auto* const startup_owner = startup.get();
        if (!builder.add(
                std::move(startup),
                lux::ecs::kPhaseSceneLoading))
        {
            lux::log::error(
                "scene",
                "startup EntitySection system assembly failed");
            return failBringUp();
        }

        if (const auto committed = builder.commit(); !committed)
        {
            const auto& failure = committed.error();
            lux::log::error(
                "scene",
                "system assembly rejected before commit: {} "
                "(subject='{}', detail='{}')",
                lux::ecs::toString(failure.error),
                failure.subject,
                failure.detail);
            return failBringUp();
        }

        const auto topology = candidate_schedule->compile();
        for (const auto type : topology.unknown)
        {
            lux::log::warn(
                "scene",
                "optional system ordering target '{}' is absent",
                type.name);
        }
        if (!topology.valid())
        {
            lux::log::error(
                "scene",
                "candidate system schedule rejected before publication");
            return failBringUp();
        }

        std::unique_ptr<SceneContributionHost> candidate_contribution_host;
        if (scene_contribution_catalog_)
        {
            candidate_contribution_host =
                std::make_unique<SceneContributionHost>(
                    *candidate_schedule,
                    *candidate_services,
                    *scene_contribution_catalog_,
                    events_);
            if (contribution_bootstrap &&
                !candidate_contribution_host->adoptBootstrap(
                    std::move(*contribution_bootstrap)))
            {
                lux::log::error(
                    "scene",
                    "scene contribution bootstrap ownership adoption failed");
                return failBringUp();
            }
        }

        world_ = std::move(candidate_world);
        persistent_entities_ =
            std::move(candidate_persistent_entities);
        services_ = std::move(candidate_services);
        schedule_ = std::move(candidate_schedule);
        entity_section_loader_ = loader_owner;
        startup_sections_ = startup_owner;
        entity_scene_catalog_ = catalog;
        scene_contribution_host_ = std::move(candidate_contribution_host);

        if (integration_)
        {
            SceneRuntimePublishedContext published{
                builder,
                *schedule_,
                *world_,
                *services_,
                scene_contribution_host_.get(),
                events_,
                extension_modules_,
                [this]() noexcept
                {
                    requestCloseProgress();
                }};
            if (!integration_->onPublished(published))
            {
                lux::log::error(
                    "scene",
                    "scene integration rejected publication");
                return failBringUp();
            }
        }

        state_ = ESceneRuntimeState::LOADING;
        live_ = true;
        return true;
    }

    void SceneRuntime::tick(
        float dt,
        float content_width,
        float content_height,
        const lux::input::ActionMapper& mapper,
        std::uint64_t frame_serial)
    {
        (void)content_width;
        (void)content_height;
        (void)mapper;
        if (!live_ || !schedule_)
            return;

        latest_frame_trace_ = {};
        latest_frame_trace_.frame_serial = frame_serial;
        if (state_ == ESceneRuntimeState::LOADING)
        {
            schedule_->tick(
                0.f,
                lux::ecs::kPhaseSceneLoading,
                frame_serial);
            switch (startup_sections_->state())
            {
            case entity_scene::EEntitySceneState::READY:
                state_ = ESceneRuntimeState::READY;
                break;
            case entity_scene::EEntitySceneState::FAILED:
                state_ = ESceneRuntimeState::FAILED;
                live_ = false;
                if (const auto& failure = startup_sections_->failure())
                {
                    lux::log::error(
                        "scene",
                        "startup EntitySection loading failed: {}",
                        failure->detail);
                }
                break;
            default:
                break;
            }
        }
        else if (state_ == ESceneRuntimeState::READY)
        {
            if (scene_contribution_host_)
            {
                const auto started = std::chrono::steady_clock::now();
                (void)scene_contribution_host_->processSafePoint();
                latest_frame_trace_.contribution_safe_point_nanoseconds =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count());
            }
            if (integration_)
            {
                const auto started = std::chrono::steady_clock::now();
                integration_->processSafePoint();
                latest_frame_trace_.integration_safe_point_nanoseconds =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count());
            }
            schedule_->tick(dt, lux::ecs::kPhaseLast, frame_serial);
        }

        const auto& schedule_trace = schedule_->latestFrameTrace();
        latest_frame_trace_.schedule_phase_nanoseconds =
            schedule_trace.phase_nanoseconds;
        latest_frame_trace_.command_barrier_nanoseconds =
            schedule_trace.command_barrier_nanoseconds;
    }

    std::span<const lux::ecs::ScheduleSystemFrameTrace>
    SceneRuntime::latestScheduleSystemFrameTrace() const noexcept
    {
        return schedule_ ? schedule_->latestSystemFrameTrace()
                         : std::span<
                               const lux::ecs::ScheduleSystemFrameTrace>{};
    }
}
