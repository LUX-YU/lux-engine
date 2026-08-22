#include <lux/engine/hosts/game_application/GameApplication.hpp>

#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include <lux/engine/runtime/scene/script/SceneScriptRuntime.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/render/scene/RenderSceneIntegration.hpp>
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>
#include <lux/engine/runtime/render/backend_host/RenderBackendHost.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/scene/RenderDiagnostics.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>
#include <lux/engine/ecs/animation/InstallAnimationSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallNavigation3DSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallPhysics3DSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallPresentation3DSystems.hpp>
#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>
#include <lux/engine/ecs/physics/InstallSimulationSystems.hpp>
#include <lux/engine/ecs/transform/InstallTransformSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallTilemapSystems.hpp>
#include <lux/engine/runtime/scene/composition/InstallSpatial3DSystems.hpp>
#include <lux/engine/ecs/spatial3d/streaming/SpatialInterest3DSystem.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionResidencySystem.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/PointLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SpotLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/WaterSurfaceComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/VisualLodNodeComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/script/backends/LuaScriptBackend.hpp>
#include <lux/engine/ecs/script/backends/NativeModuleScriptBackend.hpp>

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetEvents.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>
#include <lux/engine/runtime/logging/LogRouter.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/assets/AssetLoadSenders.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/assets/navigation/Navigation3DPrepareService.hpp>
#include <lux/engine/runtime/assets/physics3d/StaticCollider3DPrepareService.hpp>
#include <lux/engine/ecs/physics3d/streaming/StaticCollider3DSystem.hpp>
#include <lux/engine/runtime/assets/tilemap/TilemapPrepareService.hpp>

#include <lux/engine/input/Input.hpp>

#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/events/DomainEvents.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace lux::game
{
    namespace
    {
        struct BuiltinComponentSchemaSnapshot final
        {
            bool initialized{false};
            std::vector<lux::ecs::ComponentSchemaDescriptor> descriptors;
        };

        [[nodiscard]] BuiltinComponentSchemaSnapshot&
        builtinComponentSchemaSnapshot() noexcept
        {
            // GameApplication composition is main-thread only. Generated
            // built-in component registrars run once per process, while each
            // application instance owns a fresh ComponentTypeCatalog. Keep an
            // immutable copy so benchmark repeats and host restarts can
            // republish the built-ins without rebuilding ReflectionRegistry or
            // conflating this path with extension registration transactions.
            static BuiltinComponentSchemaSnapshot snapshot;
            return snapshot;
        }

        [[nodiscard]] lux::cxx::expected<
            std::size_t,
            lux::ecs::ComponentCatalogFailure>
        registerBuiltinComponents(lux::ecs::ComponentTypeCatalog& catalog)
        {
            if (!lux::meta::ReflectionRegistry::initialized())
                lux::meta::meta_module_init();

            auto& snapshot = builtinComponentSchemaSnapshot();
            if (!snapshot.initialized)
            {
                snapshot.descriptors =
                    lux::ecs::takeGeneratedComponents();
                snapshot.initialized = true;
            }
            return catalog.registerSchemas(snapshot.descriptors);
        }
    }

#include "GameApplication.State.inl"

    GameApplication::GameApplication() = default;
    GameApplication::~GameApplication()
    {
        if (!impl_ || impl_->close())
            return;

        // A close watchdog is an ownership-preservation boundary. Destroying
        // the composition root after it expires would invalidate the render
        // progress domain and AsyncRuntime while their accepted senders still
        // hold callbacks. Preserve the complete assembly so a late completion
        // cannot turn a loud close failure into a use-after-free during stack
        // unwinding. Process teardown will reclaim it; a live host should keep
        // the GameApplication and retry close after reporting the failure.
        (void)impl_.release();
        lux::log::error(
            "game_application",
            "application owner preserved after close watchdog expiration");
    }

    bool GameApplication::start(
        GameApplicationConfig config,
        std::uint64_t native_surface,
        lux::math::Extent2u extent)
    {
        if (impl_ && impl_->live)
            return true;

        auto next = std::make_unique<Impl>();
        auto& application = *next;
        application.config = std::move(config);
        application.events = std::make_unique<lux::events::DomainEvents>();
        application.frame_pump =
            &application.events->createPump("game_frame");

        if (application.config.game_pak_file.empty() ||
            !std::filesystem::exists(application.config.game_pak_file))
        {
            lux::log::error(
                "game_application",
                "cooked game pak '{}' is missing",
                application.config.game_pak_file.string()
            );
            return false;
        }
        if (application.config.save_root.empty())
        {
            lux::log::error(
                "game_application",
                "platform host did not provide a writable World save root");
            return false;
        }

        lux::runtime::installRenderBridgeLogging();
        if (auto registered = registerBuiltinComponents(
                application.component_types); !registered)
        {
            lux::log::error(
                "game_application",
                "generated component schema registration failed for '{}'",
                registered.error().name
            );
            return false;
        }
        if (application.component_types.all().empty())
        {
            lux::log::error(
                "game_application",
                "component catalogue is empty; reflection sidecars are missing"
            );
            return false;
        }

        if (!application.startRenderBackend())
            return false;
        lux::runtime::installRenderErrorLogging(
            *application.session,
            *application.events,
            *application.frame_pump,
            application.subs
        );

        const auto scene_codecs = lux::scene::makeSceneAssetCodecCatalog(
            *lux::asset::runtimeAssetCodecCatalog());
        if (!scene_codecs)
        {
            lux::log::error(
                "game_application",
                "Scene asset codec catalog composition failed ({})",
                static_cast<unsigned>(scene_codecs.error()));
            return false;
        }
        application.assets = std::make_shared<lux::asset::AssetManager>(*scene_codecs);
        auto vfs = std::make_shared<lux::asset::AssetVfs>();
        auto game_pak = lux::asset::PakAssetProvider::loadFromFile(
            application.config.game_pak_file
        );
        if (!game_pak)
        {
            lux::log::error(
                "game_application",
                "game pak '{}' was rejected: {}",
                application.config.game_pak_file.string(),
                game_pak.error()
            );
            return false;
        }
        lux::log::info(
            "game_application",
            "/Game <- '{}' ({} entries)",
            application.config.game_pak_file.string(),
            game_pak.value()->assetCount()
        );
        vfs->mount({"/Game", game_pak.value(), 0});

        if (!application.config.base_pak_file.empty())
        {
            if (!std::filesystem::exists(
                    application.config.base_pak_file))
            {
                lux::log::error(
                    "game_application",
                    "base-content pak '{}' is missing",
                    application.config.base_pak_file.string()
                );
                return false;
            }
            auto base_pak = lux::asset::PakAssetProvider::loadFromFile(
                application.config.base_pak_file
            );
            if (!base_pak)
            {
                lux::log::error(
                    "game_application",
                    "base-content pak '{}' was rejected: {}",
                    application.config.base_pak_file.string(),
                    base_pak.error()
                );
                return false;
            }
            // Cooked assets still use the legacy /Engine virtual root. The public
            // launch/configuration vocabulary is product-neutral; migrating persisted
            // virtual paths is a separate format transition.
            vfs->mount({"/Engine", base_pak.value(), 0});
        }
        application.assets->setVfs(std::move(vfs));

        lux::exec::AsyncRuntimeBuilder async_builder;
        auto asset_load = lux::asset_runtime::AssetLoadService::addTo(
            async_builder,
            *application.assets
        );
        if (!asset_load)
        {
            lux::log::error(
                "game_application",
                "asset async feature assembly failed"
            );
            return false;
        }
        auto entity_sections =
            lux::runtime::entity_scene::EntitySectionService::addTo(
                async_builder);
        if (!entity_sections)
        {
            lux::log::error(
                "game_application",
                "EntitySection async feature assembly failed");
            return false;
        }
        auto upload_service =
            lux::runtime::AsyncRenderUploadService::addTo(async_builder);
        if (!upload_service)
        {
            lux::log::error(
                "game_application",
                "upload async feature assembly failed"
            );
            return false;
        }
        auto geometry_preparation =
            lux::runtime::SceneGeometryPrepareService::addTo(async_builder);
        if (!geometry_preparation)
        {
            lux::log::error(
                "game_application",
                "scene geometry preparation assembly failed");
            return false;
        }
        auto navigation_preparation = lux::runtime::assets::navigation::
            Navigation3DPrepareService::addTo(async_builder);
        if (!navigation_preparation)
        {
            lux::log::error(
                "game_application",
                "navigation preparation assembly failed");
            return false;
        }
        auto physics_preparation = lux::runtime::assets::physics3d::
            StaticCollider3DPrepareService::addTo(async_builder);
        if (!physics_preparation)
        {
            lux::log::error(
                "game_application",
                "static collider preparation assembly failed");
            return false;
        }
        auto tilemap_preparation = lux::runtime::assets::tilemap::
            TilemapPrepareService::addTo(async_builder);
        if (!tilemap_preparation)
        {
            lux::log::error(
                "game_application",
                "Tilemap preparation assembly failed");
            return false;
        }
        auto async_plan = std::move(async_builder).compile();
        if (!async_plan)
        {
            lux::log::error(
                "game_application",
                "async runtime assembly failed"
            );
            return false;
        }
        application.async = std::make_unique<lux::exec::AsyncRuntime>(
            std::move(*async_plan),
            lux::exec::AsyncRuntimeConfig{
                .blocking_io_threads =
                    application.config.blocking_io_threads,
                .background_cpu_concurrency =
                    application.config.background_cpu_concurrency
            }
        );
        application.frame_coordinator =
            std::make_unique<lux::runtime::FrameCoordinator>(
                *application.session,
                *application.control,
                *application.frame_pump,
                *application.async
            );
        application.upload_service = std::make_unique<
            lux::runtime::AsyncRenderUploadService>(
                std::move(*upload_service)
            );
        if (!application.upload_service->bind(
                *application.async,
                application.render_host.uploadSession(),
                application.render_host.sync()
            ))
        {
            lux::log::error(
                "game_application",
                "upload coordinator bind failed"
            );
            return false;
        }
        application.log_router =
            std::make_unique<lux::logging::LogRouter>(*application.async);
        application.log_router->install();
        application.asset_load = std::make_unique<
            lux::asset_runtime::AssetLoadService>(std::move(*asset_load));
        application.entity_sections = std::make_unique<
            lux::runtime::entity_scene::EntitySectionService>(
                std::move(*entity_sections));
        application.geometry_preparation = std::make_unique<
            lux::runtime::SceneGeometryPrepareService>(
                std::move(*geometry_preparation));
        application.navigation_preparation = std::make_unique<
            lux::runtime::assets::navigation::Navigation3DPrepareService>(
                std::move(*navigation_preparation));
        application.physics_preparation = std::make_unique<
            lux::runtime::assets::physics3d::StaticCollider3DPrepareService>(
                std::move(*physics_preparation));
        application.tilemap_preparation = std::make_unique<
            lux::runtime::assets::tilemap::TilemapPrepareService>(
                std::move(*tilemap_preparation));

        application.extensions =
            std::make_unique<lux::extensions::EngineExtensions>(
                lux::extensions::EngineExtensionServices{
                    application.extension_modules,
                    *application.async,
                    application.component_types,
                    application.events.get(),
                    {}
                },
                application.config.extensions
            );
        if (!application.loadConfiguredExtensions())
            return false;

        if (!application.attachSurface(native_surface, extent))
            return false;

        if (application.config.boot_scene.empty())
        {
            lux::log::error(
                "game_application",
                "RuntimeLaunchManifest must specify boot_scene");
            return false;
        }

        const auto boot_id = game_pak.value()->resolve(
            application.config.boot_scene);
        if (!boot_id)
        {
            lux::log::error(
                "game_application",
                "scene vpath '{}' was not found in the game pak",
                application.config.boot_scene);
            return false;
        }
        bool boot_is_scene = false;
        game_pak.value()->enumerate(
            [&](const lux::asset::ProviderEntry& entry)
            {
                if (entry.id != *boot_id)
                    return;
                const auto* descriptor =
                    application.assets->codecCatalog().findByMagic(
                        entry.magic_number);
                boot_is_scene = descriptor != nullptr &&
                    descriptor->type == lux::scene::kSceneAssetType;
            });
        if (!boot_is_scene)
        {
            lux::log::error(
                "game_application",
                "boot vpath '{}' is not a SceneAsset",
                application.config.boot_scene);
            return false;
        }

        struct BootSceneLoad final
        {
            std::atomic<bool> done{false};
            std::atomic<bool> loaded{false};
            std::atomic<int> error{0};
        } boot_load;
        std::thread boot_waiter(
            [client = application.asset_load->client(),
             id = *boot_id,
             &boot_load]() mutable
            {
                auto terminal = stdexec::sync_wait(
                    lux::asset_runtime::loadAsset(client, id));
                if (terminal)
                {
                    auto& outcome = std::get<0>(*terminal);
                    if (outcome)
                    {
                        boot_load.loaded.store(
                            true, std::memory_order_relaxed);
                    }
                    else if (!outcome.error().isRuntime())
                    {
                        boot_load.error.store(
                            static_cast<int>(
                                outcome.error().domainError()),
                            std::memory_order_relaxed);
                    }
                }
                boot_load.done.store(true, std::memory_order_release);
            });
        while (!boot_load.done.load(std::memory_order_acquire))
        {
            (void)application.async->drainMainThreadCompletions();
            std::this_thread::yield();
        }
        boot_waiter.join();
        if (!boot_load.loaded.load(std::memory_order_relaxed))
        {
            lux::log::error(
                "game_application",
                "cannot load boot SceneAsset (error={})",
                boot_load.error.load(std::memory_order_relaxed));
            return false;
        }

        const auto scene_origin = application.config.boot_scene + " @ " +
            application.config.game_pak_file.filename().string();
        lux::log::info(
            "game_application",
            "boot scene: {}",
            scene_origin
        );

        application.input = std::make_unique<lux::input::Input>();
        if (application.config.hooks.configure_input &&
            !application.config.hooks.configure_input(
                *application.input
            ))
        {
            lux::log::error(
                "game_application",
                "compiled game input registration failed"
            );
            return false;
        }

        application.assets->setBroadcast({
            .on_unreferenced =
                [events = application.events.get()](
                    const lux::asset::asset_id_t& id)
                { events->publish(lux::asset::AssetUnreferenced{id}); },
            .on_invalidated =
                [events = application.events.get()](
                    const lux::asset::asset_id_t& id)
                { events->publish(lux::asset::AssetInvalidated{id}); },
            .on_content_changed =
                [events = application.events.get()](
                    const lux::asset::asset_id_t& id,
                    std::uint32_t revision)
                {
                    events->publish(
                        lux::asset::AssetContentChanged{id, revision}
                    );
                },
            .on_registered =
                [events = application.events.get()](
                    const lux::asset::asset_id_t& id)
                { events->publish(lux::asset::AssetRegistered{id}); }
        });
        application.residency =
            std::make_unique<lux::runtime::ResidencyAssembly>(
                *application.control,
                application.upload_service->client(),
                *application.assets,
                application.render_host.featureCatalog(),
                application.asset_load->client(),
                *application.async,
                [events = application.events.get()](
                    const lux::ecs::RenderResourceFailed& failure)
                { events->publish(failure); },
                application.config.texture_streaming
            );
        auto residency_events =
            application.residency->makeAssetEventCallbacks();
        application.subs.add(
            application.events->subscribe<lux::asset::AssetInvalidated>(
                *application.frame_pump,
                [callback = std::move(residency_events.invalidated)](
                    const lux::asset::AssetInvalidated& event) mutable
                { callback(event.id); }
            )
        );
        application.subs.add(
            application.events->subscribe<lux::asset::AssetContentChanged>(
                *application.frame_pump,
                [callback = std::move(residency_events.content_changed)](
                    const lux::asset::AssetContentChanged& event) mutable
                { callback(event.id); }
            )
        );
        application.subs.add(
            application.events->subscribe<lux::asset::AssetUnreferenced>(
                *application.frame_pump,
                [callback = std::move(residency_events.unreferenced)](
                    const lux::asset::AssetUnreferenced& event) mutable
                { callback(event.id); }
            )
        );
        application.subs.add(
            application.events->subscribe<lux::asset::AssetRegistered>(
                *application.frame_pump,
                [callback = std::move(residency_events.registered)](
                    const lux::asset::AssetRegistered& event) mutable
                { callback(event.id); }
            )
        );

        lux::runtime::SceneRuntime::Config runtime_config;
        runtime_config.name = application.config.title;
        runtime_config.scene_asset_id = *boot_id;
        runtime_config.scene_origin = scene_origin;
        runtime_config.events = application.events.get();
        runtime_config.install_systems = [&application](
            lux::ecs::ScheduleBuilder& builder)
        {
            // Ordinary code composition over the only Schedule. Ordering here
            // is construction ordering for typed service borrows; execution
            // ordering remains exclusively declared by each ISystem.
            return lux::ecs::installSpatial3DTransformSystems(
                       builder,
                       application.component_types) &&
                lux::ecs::installAnimation3DSystems(
                    builder,
                    application.component_types) &&
                lux::runtime::installPhysics3DSystems(
                    builder,
                    application.component_types,
                    application.physics_preparation->client()) &&
                lux::runtime::installNavigation3DSystems(
                    builder,
                    application.component_types,
                    application.navigation_preparation->client()) &&
                lux::ecs::installSpatial2DTransformSystems(
                    builder,
                    application.component_types) &&
                lux::ecs::installSimulation2DSystems(
                    builder,
                    application.component_types) &&
                lux::runtime::installTilemap2DSystems(
                    builder,
                    application.component_types,
                    application.tilemap_preparation->client()) &&
                lux::runtime::installSpatial3DSystems(
                    builder,
                    application.component_types) &&
                lux::runtime::installPresentation3DSystems(
                    builder,
                    application.component_types,
                    application.geometry_preparation->classicMeshClient(),
                    application.geometry_preparation->terrainClient()) &&
                lux::ecs::installPresentation2DSystems(
                    builder,
                    application.component_types);
        };

        lux::runtime::RenderSceneServices render_services{
            .frame = *application.session,
            .control = *application.control,
            .upload = application.upload_service->client(),
            .feature_catalog = application.render_host.featureCatalog(),
            .feature_plan = application.render_host.featurePlan(),
            .residency = *application.residency,
            .profile = lux::runtime::standardDesktopProfile()
        };
        const lux::runtime::SceneRuntime::Dependencies dependencies{
            .assets = *application.assets,
            .asset_client = application.asset_load->client(),
            .async = *application.async,
            .components = application.component_types,
            .entity_sections = application.entity_sections->loadClient(),
            .extension_modules = &application.extension_modules
        };
        application.runtime = lux::runtime::SceneRuntime::create(
            dependencies,
            runtime_config,
            std::make_unique<lux::runtime::RenderSceneIntegration>(
                render_services,
                lux::runtime::RenderSceneConfig{
                    .target = application.surface_target.id(),
                    .extent = extent,
                    .present_primary_camera = true
                }
            )
        );
        if (!application.runtime)
        {
            lux::log::error(
                "game_application",
                "scene runtime bring-up failed"
            );
            return false;
        }

        application.simulation =
            std::make_unique<lux::runtime::SceneScriptRuntime>(
                application.runtime->world(),
                application.runtime->schedule(),
                application.runtime->services(),
                *application.assets,
                application.asset_load->client()
            );
        const bool script_backends_ready =
            application.simulation->addBackend(
                std::make_unique<lux::ecs::LuaScriptBackend>(
                    application.component_types
                )
            )
            && application.simulation->addBackend(
                std::make_unique<lux::ecs::NativeModuleScriptBackend>()
            );
        if (!script_backends_ready || !application.simulation->start(
                application.input->mapper(),
                &application.input->actionRegistry()
            ))
        {
            lux::log::error(
                "game_application",
                "scene simulation start failed"
            );
            return false;
        }

        application.live = true;
        impl_ = std::move(next);
        return true;
    }

    bool GameApplication::attachSurface(
        std::uint64_t native_surface,
        lux::math::Extent2u extent)
    {
        return impl_ && impl_->attachSurface(native_surface, extent);
    }

    bool GameApplication::detachSurface() noexcept
    {
        return !impl_ || impl_->detachSurface();
    }

    bool GameApplication::tick(float dt, lux::math::Extent2u extent)
    {
        if (!impl_ || !impl_->live || !impl_->surface_target ||
            !impl_->runtime)
        {
            return false;
        }
        auto frame = impl_->frame_coordinator->begin();
        if (!frame)
        {
            const auto& stats = impl_->frame_coordinator->statistics();
            const auto terminal = impl_->session->terminalError();
            if (terminal.ok())
            {
                lux::log::error(
                    "game_application",
                    "frame begin failed (opened={}, start_failures={}, "
                    "submitted={}, submit_failures={})",
                    stats.opened,
                    stats.start_failures,
                    stats.submitted,
                    stats.submit_failures
                );
            }
            else
            {
                lux::log::error(
                    "game_application",
                    "frame begin failed (opened={}, start_failures={}, "
                    "submitted={}, submit_failures={}, terminal={})",
                    stats.opened,
                    stats.start_failures,
                    stats.submitted,
                    stats.submit_failures,
                    lux::render::formatRenderError(
                        lux::render::renderErrorRegistry(),
                        terminal)
                );
            }
            return false;
        }
        const auto streaming_started = std::chrono::steady_clock::now();
        impl_->residency->tickTextureStreaming();
        frame.recordPhase(
            lux::runtime::EFrameTracePhase::TEXTURE_STREAMING,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - streaming_started)
                    .count()));
        const float frame_dt = lux::runtime::clampFrameDt(dt);
        frame.record(
            [&]
            {
                impl_->runtime->tick(
                    frame_dt,
                    static_cast<float>(extent.width),
                    static_cast<float>(extent.height),
                    impl_->input->mapper(),
                    frame.sequence()
                );
                impl_->observeVisualRevisions();
                const auto& trace = impl_->runtime->latestFrameTrace();
                frame.recordPhase(
                    lux::runtime::EFrameTracePhase::
                        INTEGRATION_SAFE_POINT,
                    trace.integration_safe_point_nanoseconds);
                constexpr std::array phases{
                    lux::runtime::EFrameTracePhase::SCHEDULE_INPUT,
                    lux::runtime::EFrameTracePhase::SCHEDULE_PRE_TRANSFORM,
                    lux::runtime::EFrameTracePhase::SCHEDULE_SIMULATION,
                    lux::runtime::EFrameTracePhase::SCHEDULE_PRE_RENDER,
                    lux::runtime::EFrameTracePhase::SCHEDULE_RENDER,
                    lux::runtime::EFrameTracePhase::SCHEDULE_POST_RENDER};
                for (std::size_t index = 0u; index < phases.size(); ++index)
                {
                    frame.recordPhase(
                        phases[index],
                        trace.schedule_phase_nanoseconds[index]);
                }
                frame.recordPhase(
                    lux::runtime::EFrameTracePhase::COMMAND_BARRIER,
                    trace.command_barrier_nanoseconds);
            }
        );
        return true;
    }

    std::size_t GameApplication::pumpSafePoint()
    {
        if (!impl_ || !impl_->frame_coordinator)
            return 0u;
        return impl_->frame_coordinator->pumpSafePoint();
    }

    bool GameApplication::sceneReady() const noexcept
    {
        return impl_ && impl_->live && impl_->runtime &&
            impl_->runtime->isReady();
    }

    std::size_t GameApplication::pumpIdleFor(std::chrono::steady_clock::duration max_wait)
    {
        if (!impl_ || !impl_->frame_coordinator)
            return 0u;
        const auto observed = impl_->frame_coordinator->observeProgress();
        const auto work = impl_->frame_coordinator->pumpSafePoint();
        if (work == 0u)
        {
            (void)impl_->frame_coordinator->waitForProgressUntil(
                observed,
                std::chrono::steady_clock::now() + max_wait
            );
        }
        return work;
    }

    void GameApplication::bindExternalWake(std::function<void()> wake)
    {
        if (!impl_ || !impl_->async || !wake)
            return;
        impl_->unbindExternalWake();
        auto context = std::make_shared<Impl::ExternalWakeContext>(
            Impl::ExternalWakeContext{
                impl_->render_host.sync(),
                std::move(wake)
            }
        );
        impl_->external_wake = std::make_shared<
            lux::exec::MainThreadMailbox::WakeBinding>(
                lux::exec::MainThreadMailbox::WakeBinding{
                    std::move(context),
                    [](void* opaque) noexcept
                    {
                        auto& context = *static_cast<
                            Impl::ExternalWakeContext*>(opaque);
                        context.render_sync->notifyRequestStateChanged();
                        context.platform_wake();
                    }
                }
            );
        impl_->async->mainThreadMailbox().bindExternalWake(
            impl_->external_wake
        );
    }

    void GameApplication::unbindExternalWake() noexcept
    {
        if (impl_)
            impl_->unbindExternalWake();
    }

    std::optional<std::string> GameApplication::renderGraphDump()
    {
        if (!impl_ || !impl_->live || !impl_->runtime)
            return std::nullopt;
        // Large GPU-driven scenes can contribute thousands of MDC bucket lanes;
        // their useful resource/barrier section appears after that table.  A 64 KiB
        // buffer silently truncated the diagnostic exactly before the information
        // needed to identify target-layout faults.
        std::string buffer(2u * 1024u * 1024u, '\0');
        lux::render::RenderGraphDumpReply reply{};
        if (!impl_->settle(
                impl_->control->dumpRenderGraph(
                    lux::runtime::renderScene(*impl_->runtime)->sceneId(),
                    buffer.data(),
                    buffer.size()
                ),
                reply,
                "dumpRenderGraph"
            ))
        {
            return std::nullopt;
        }
        buffer.resize(std::min<std::size_t>(reply.written, buffer.size()));
        return buffer;
    }

    std::optional<std::string> GameApplication::gpuTimingDump()
    {
        if (!impl_ || !impl_->live || !impl_->runtime)
            return std::nullopt;

        std::string buffer(64u * 1024u, '\0');
        lux::render::GpuTimingReply reply{};
        if (!impl_->settle(
                impl_->control->queryGpuTiming(
                    lux::runtime::renderScene(*impl_->runtime)->sceneId(),
                    buffer.data(),
                    buffer.size()
                ),
                reply,
                "queryGpuTiming"
            ))
        {
            return std::nullopt;
        }
        if (reply.needed > buffer.size())
        {
            buffer.assign(reply.needed, '\0');
            if (!impl_->settle(
                    impl_->control->queryGpuTiming(
                        lux::runtime::renderScene(*impl_->runtime)->sceneId(),
                        buffer.data(),
                        buffer.size()
                    ),
                    reply,
                    "queryGpuTiming(retry)"
                ))
            {
                return std::nullopt;
            }
        }
        buffer.resize(std::min<std::size_t>(reply.written, buffer.size()));
        return buffer;
    }

    std::optional<lux::runtime::FrameTrace>
    GameApplication::latestFrameTrace() const noexcept
    {
        return impl_ && impl_->frame_coordinator
            ? impl_->frame_coordinator->latestTrace()
            : std::nullopt;
    }

    std::span<const lux::ecs::ScheduleSystemFrameTrace>
    GameApplication::latestScheduleSystemFrameTrace() const noexcept
    {
        return impl_ && impl_->runtime
            ? impl_->runtime->latestScheduleSystemFrameTrace()
            : std::span<const lux::ecs::ScheduleSystemFrameTrace>{};
    }

    std::vector<lux::runtime::FrameTrace>
    GameApplication::frameTraceHistory() const
    {
        return impl_ && impl_->frame_coordinator
            ? impl_->frame_coordinator->traceHistory()
            : std::vector<lux::runtime::FrameTrace>{};
    }

    const lux::render::CapacityPlan&
    GameApplication::capacityPlan() const noexcept
    {
        return impl_->capacity_plan;
    }

    const std::optional<lux::render::CapacityShortfall>&
    GameApplication::capacityShortfall() const noexcept
    {
        return impl_->capacity_shortfall;
    }

    bool GameApplication::setMainCameraPose(
        const GameApplicationCameraPose& pose) noexcept
    {
        return impl_ && impl_->live && impl_->setMainCameraPose(pose);
    }

    bool GameApplication::setMainCameraClipRange(
        float near_z,
        float far_z) noexcept
    {
        return impl_ && impl_->live &&
            impl_->setMainCameraClipRange(near_z, far_z);
    }

    bool GameApplication::patchVisualState(
        const GameApplicationVisualPatch& patch) noexcept
    {
        return impl_ && impl_->live && impl_->patchVisualState(patch);
    }

    std::optional<GameApplicationVisualState>
    GameApplication::visualState() const noexcept
    {
        return impl_ && impl_->live
            ? impl_->visualState()
            : std::nullopt;
    }

    std::optional<lux::navigation::NavigationPathResult>
    GameApplication::queryNavigationPath(
        const lux::navigation::NavigationPathRequest& request) noexcept
    {
        if (!impl_ || !impl_->live || !impl_->runtime ||
            !lux::navigation::valid(request))
            return std::nullopt;
        const auto* service = impl_->runtime->services().get<
            lux::ecs::NavigationQueryService>();
        if (!service)
            return std::nullopt;

        auto result = service->query(request);
        auto& state = impl_->navigation_queries;
        ++state.submitted;
        ++state.completed;
        state.last_path_points =
            static_cast<std::uint32_t>(result.points.size());
        state.last_missing_regions =
            static_cast<std::uint32_t>(result.missing_regions.size());
        switch (result.status)
        {
        case lux::navigation::ENavigationPathStatus::COMPLETE:
            ++state.complete_paths;
            break;
        case lux::navigation::ENavigationPathStatus::PARTIAL:
            ++state.partial_paths;
            break;
        case lux::navigation::ENavigationPathStatus::PENDING:
            ++state.pending_paths;
            if (result.missing_regions.empty())
                ++state.failed;
            break;
        case lux::navigation::ENavigationPathStatus::FAILED:
            ++state.failed;
            break;
        }
        return result;
    }

    std::optional<GameApplicationTelemetry>
    GameApplication::telemetrySnapshot()
    {
        if (!impl_ || !impl_->live || !impl_->runtime || !impl_->control)
            return std::nullopt;

        GameApplicationTelemetry telemetry;
        auto& registry = impl_->runtime->world().registry();

        const auto camera_entity = impl_->mainCamera();
        if (camera_entity != entt::null && registry.valid(camera_entity))
        {
            telemetry.spatial3d_camera_interest = registry.all_of<
                lux::ecs::SpatialInterest3DComponent>(camera_entity);
            if (const auto* transform = registry.try_get<
                    lux::ecs::ResolvedTransform3DComponent>(camera_entity))
            {
                telemetry.camera_position_x = transform->position.x;
                telemetry.camera_position_y = transform->position.y;
                telemetry.camera_position_z = transform->position.z;
                const Eigen::Vector3f forward =
                    -transform->linear.col(2).normalized();
                telemetry.camera_forward_x = forward.x();
                telemetry.camera_forward_y = forward.y();
                telemetry.camera_forward_z = forward.z();
                telemetry.camera_pose_valid = forward.allFinite();
            }
        }

        telemetry.spatial3d_catalog_present =
            !impl_->runtime->entityScene().package()
                 .spatial3d_catalog.empty();
        if (const auto* interest = impl_->runtime->services().get<
                lux::ecs::spatial3d::streaming::SpatialInterest3DSystem>())
        {
            const auto snapshot = interest->snapshot();
            telemetry.spatial3d_interest_available = true;
            telemetry.spatial3d_tracked_sources = snapshot.tracked_sources;
            telemetry.spatial3d_active_sections = snapshot.active_sections;
            telemetry.spatial3d_resident_sections =
                snapshot.resident_sections;
        }
        if (const auto* partition = impl_->runtime->services().get<
                lux::ecs::entity_scene::residency::EntitySectionResidencySystem>())
        {
            const auto snapshot = partition->snapshot();
            telemetry.spatial3d_waiting_sections =
                snapshot.waiting_sections;
            telemetry.spatial3d_staging_sections =
                snapshot.staging_sections;
            telemetry.spatial3d_published_sections =
                snapshot.active_sections;
            telemetry.spatial3d_failed_sections =
                snapshot.failed_sections;
        }

        if (impl_->frame_coordinator)
        {
            const auto& stats = impl_->frame_coordinator->statistics();
            telemetry.frame_opened = stats.opened;
            telemetry.frame_submitted = stats.submitted;
            telemetry.frame_slot_wait_nanoseconds =
                stats.frame_slot_wait_ns;
            telemetry.frame_slot_wait_max_nanoseconds =
                stats.frame_slot_wait_max_ns;
        }
        telemetry.validation_error_count = static_cast<std::uint32_t>(
            std::max(0, impl_->validation_error_count.load(
                std::memory_order_relaxed)));
        if (impl_->async)
        {
            const auto stats = impl_->async->stats();
            telemetry.async_accepted = stats.accepted;
            telemetry.async_rejected = stats.rejected;
            telemetry.async_active_operations = stats.active_operations;
            telemetry.async_queue_high_water = stats.queue_high_water;
            telemetry.async_byte_high_water = stats.byte_high_water;
            telemetry.async_main_queue_high_water =
                stats.main_queue_high_water;
            telemetry.async_coordinator_handler_nanoseconds =
                stats.coordinator_handler_total_ns;
            telemetry.async_coordinator_handler_max_nanoseconds =
                stats.coordinator_handler_max_ns;
            telemetry.async_blocking_io_running =
                stats.blocking_io_running;
            telemetry.async_background_cpu_running =
                stats.background_cpu_running;
        }
        if (impl_->upload_service)
        {
            const auto client = impl_->upload_service->client();
            const auto client_stats = client.statistics();
            const auto service_stats = impl_->upload_service->report();
            telemetry.upload_submitted_packets =
                client_stats.submitted_packets;
            telemetry.upload_shared_bytes =
                client_stats.payload_shared_bytes;
            telemetry.upload_copied_bytes =
                client_stats.payload_copied_bytes;
            telemetry.upload_pending_backpressure =
                service_stats.pending_backpressure;
            telemetry.upload_active_replies = service_stats.active_replies;
            telemetry.upload_accepted_inflight =
                service_stats.accepted_inflight;
            telemetry.upload_retry_attempts = service_stats.retry_attempts;
            telemetry.upload_retry_high_water =
                service_stats.retry_high_water;
        }
        if (const auto channel = impl_->render_host.uploadChannel())
        {
            telemetry.upload_queue_high_water = channel->queueHighWater();
            telemetry.upload_payload_high_water = channel->payloadHighWater();
        }
        if (const auto* physics = impl_->runtime->services().get<
                lux::ecs::physics3d::streaming::Physics3DSceneService>();
            physics && physics->scene)
        {
            const auto stats = physics->snapshot();
            telemetry.physics_dynamic_bodies = stats.dynamic_body_count;
            telemetry.physics_characters = stats.character_count;
            telemetry.physics_static_heightfield_bodies =
                stats.static_heightfield_body_count;
            telemetry.physics_capacity_bytes = stats.capacity_bytes;
            telemetry.physics_allocation_count = stats.allocation_count;
        }
        if (const auto* navigation = impl_->runtime->services().get<
                lux::ecs::NavigationQueryService>())
        {
            const auto stats = navigation->lifecycleSnapshot();
            telemetry.navigation_generation = stats.generation;
            telemetry.navigation_waiting_regions = stats.waiting_regions;
            telemetry.navigation_staging_regions = stats.staging_regions;
            telemetry.navigation_ready_regions = stats.ready_regions;
            telemetry.navigation_active_regions = stats.active_regions;
            telemetry.navigation_retiring_regions = stats.retiring_regions;
            telemetry.navigation_requests_emitted = stats.requests_emitted;
            telemetry.navigation_queue_backpressure =
                stats.queue_backpressure;
            telemetry.navigation_stale_completions =
                stats.stale_completions;
            telemetry.navigation_failed_regions = stats.failed_regions;
            telemetry.navigation_staging_work_items =
                stats.staging_work_items;
            telemetry.navigation_retirement_work_items =
                stats.retirement_work_items;
            telemetry.navigation_staging_bytes = stats.staging_bytes;
            telemetry.navigation_retired_bytes = stats.retired_bytes;
            telemetry.navigation_close_hides = stats.close_hides;
            telemetry.navigation_owner_bytes = stats.owner_bytes;
            telemetry.navigation_maximum_staging_work_items_per_tick =
                stats.maximum_staging_work_items_per_tick;
            telemetry.navigation_maximum_retirement_work_items_per_tick =
                stats.maximum_retirement_work_items_per_tick;
            telemetry.navigation_maximum_close_hides_per_tick =
                stats.maximum_close_hides_per_tick;
        }
        {
            const auto& stats = impl_->navigation_queries;
            telemetry.navigation_queries_submitted = stats.submitted;
            telemetry.navigation_queries_completed = stats.completed;
            telemetry.navigation_queries_failed = stats.failed;
            telemetry.navigation_queries_complete_paths =
                stats.complete_paths;
            telemetry.navigation_queries_partial_paths =
                stats.partial_paths;
            telemetry.navigation_queries_pending_paths =
                stats.pending_paths;
            telemetry.navigation_last_path_points =
                stats.last_path_points;
            telemetry.navigation_last_missing_regions =
                stats.last_missing_regions;
        }
        telemetry.root_3d_transforms = 0u;
        for (const auto entity : registry.view<
                 const lux::ecs::Transform3DComponent>(
                 entt::exclude<lux::ecs::ParentComponent>))
        {
            (void)entity;
            ++telemetry.root_3d_transforms;
        }
        telemetry.point_lights = registry.storage<
            lux::ecs::PointLightComponent>().size();
        telemetry.spot_lights = registry.storage<
            lux::ecs::SpotLightComponent>().size();
        telemetry.directional_lights = registry.storage<
            lux::ecs::DirectionalLightComponent>().size();
        telemetry.water_surfaces = registry.storage<
            lux::ecs::WaterSurfaceComponent>().size();
        telemetry.rigid_bodies_3d = registry.storage<
            lux::ecs::RigidBody3DComponent>().size();
        telemetry.character_controllers_3d = registry.storage<
            lux::ecs::CharacterController3DComponent>().size();
        telemetry.sky_revision = impl_->sky_revision;
        telemetry.directional_light_revision =
            impl_->directional_light_revision;
        telemetry.height_fog_revision = impl_->height_fog_revision;

        // EntityScene publication makes the registry the only scene-content
        // truth. Derive the diagnostic Classic/HLOD split and registry-space
        // bounds from ordinary ECS facts instead of consulting the retired
        // parallel render-cluster/page-local representation.
        const auto include_bounds = [&telemetry](
            double x,
            double y,
            double z,
            double radius) noexcept
        {
            if (!std::isfinite(x) || !std::isfinite(y) ||
                !std::isfinite(z) || !std::isfinite(radius) || radius < 0.0)
            {
                return;
            }
            const double minimum_x = x - radius;
            const double minimum_y = y - radius;
            const double minimum_z = z - radius;
            const double maximum_x = x + radius;
            const double maximum_y = y + radius;
            const double maximum_z = z + radius;
            if (!telemetry.world_render_bounds_valid)
            {
                telemetry.world_render_min_x = minimum_x;
                telemetry.world_render_min_y = minimum_y;
                telemetry.world_render_min_z = minimum_z;
                telemetry.world_render_max_x = maximum_x;
                telemetry.world_render_max_y = maximum_y;
                telemetry.world_render_max_z = maximum_z;
                telemetry.world_render_bounds_valid = true;
                return;
            }
            telemetry.world_render_min_x = std::min(
                telemetry.world_render_min_x, minimum_x);
            telemetry.world_render_min_y = std::min(
                telemetry.world_render_min_y, minimum_y);
            telemetry.world_render_min_z = std::min(
                telemetry.world_render_min_z, minimum_z);
            telemetry.world_render_max_x = std::max(
                telemetry.world_render_max_x, maximum_x);
            telemetry.world_render_max_y = std::max(
                telemetry.world_render_max_y, maximum_y);
            telemetry.world_render_max_z = std::max(
                telemetry.world_render_max_z, maximum_z);
        };
        for (const auto entity : registry.view<
                 const lux::ecs::ClassicMeshBatchComponent,
                 const lux::ecs::ResolvedTransform3DComponent>())
        {
            const auto& batch = registry.get<
                lux::ecs::ClassicMeshBatchComponent>(entity);
            const auto& transform = registry.get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            const auto* lod = registry.try_get<
                lux::ecs::VisualLodNodeComponent>(entity);
            auto& cluster_count = lod && lod->level != 0u
                ? telemetry.world_hlod_render_clusters
                : telemetry.world_classic_render_clusters;
            ++cluster_count;

            const Eigen::Vector3f center_offset =
                transform.linear * batch.local_bounds_center;
            const float maximum_scale = std::max({
                transform.linear.col(0).norm(),
                transform.linear.col(1).norm(),
                transform.linear.col(2).norm()});
            include_bounds(
                transform.position.x +
                    static_cast<double>(center_offset.x()),
                transform.position.y +
                    static_cast<double>(center_offset.y()),
                transform.position.z +
                    static_cast<double>(center_offset.z()),
                static_cast<double>(batch.local_bounds_radius) *
                    static_cast<double>(maximum_scale));
        }

        const auto scene = lux::runtime::renderScene(
            *impl_->runtime)->sceneId();
        auto& catalog = impl_->render_host.featureCatalog();

        const auto cluster_ops = catalog.ops<
            lux::render::RenderClusterOperationIds>("RenderCluster");
        if (cluster_ops.valid())
        {
            lux::render::RenderClusterStatsReply reply{};
            if (impl_->settle(
                    lux::render::RenderClusterControlClient{
                        *impl_->control, cluster_ops}
                        .stats({scene}),
                    reply,
                    "RenderClusterStats"))
            {
                telemetry.render_cluster_available = true;
                telemetry.render_clusters = reply.cluster_count;
                telemetry.render_instances = reply.instance_count;
                telemetry.visible_render_clusters =
                    reply.visible_cluster_count;
                telemetry.visible_render_instances =
                    reply.visible_instance_count;
                telemetry.gpu_render_candidates = reply.gpu_candidate_count;
                telemetry.gpu_render_candidates_requested =
                    reply.gpu_candidate_requested_count;
                telemetry.gpu_render_candidates_overflow =
                    reply.gpu_candidate_overflow_count;
                telemetry.gpu_render_candidate_groups =
                    reply.gpu_candidate_group_count;
                telemetry.gpu_render_candidates_valid =
                    reply.gpu_candidate_count_valid != 0u;
                telemetry.cull_visible_flag_instances =
                    reply.cull_visible_flag_count;
                telemetry.cull_gbuffer_pass_instances =
                    reply.cull_gbuffer_pass_count;
                telemetry.cull_geometry_instances =
                    reply.cull_geometry_count;
                telemetry.cull_lod_instances = reply.cull_lod_count;
                telemetry.cull_mdc_instances = reply.cull_mdc_count;
                telemetry.cull_frustum_instances =
                    reply.cull_frustum_count;
                telemetry.non_white_render_instances =
                    reply.non_white_instance_count;
                telemetry.render_instance_rgba8_xor =
                    reply.instance_rgba8_xor;
                telemetry.wanted_mip_textures =
                    reply.wanted_mip_texture_count;
                telemetry.minimum_wanted_mip = reply.minimum_wanted_mip;
                telemetry.workgroup_aggregation_fallbacks =
                    reply.workgroup_aggregation_fallback_count;
                telemetry.wanted_mip_feedback_valid =
                    reply.wanted_mip_feedback_valid != 0u;
                telemetry.texture_full_bytes = reply.full_texture_bytes;
                telemetry.texture_target_bytes = reply.target_texture_bytes;
                telemetry.texture_actual_bytes = reply.actual_texture_bytes;
                telemetry.render_cluster_cpu_capacity_bytes =
                    reply.cpu_capacity_bytes;
                telemetry.render_cluster_cpu_allocation_count =
                    reply.cpu_allocation_count;
            }
        }

        const auto mesh_stack_ops = catalog.ops<
            lux::render::MeshStackOperationIds>("StandardMeshStack");
        if (mesh_stack_ops.valid())
        {
            lux::render::MeshStackStatsReply reply{};
            if (impl_->settle(
                    lux::render::MeshStackControlClient{
                        *impl_->control, mesh_stack_ops}
                        .stats({scene}),
                    reply,
                    "MeshStackStats"))
            {
                telemetry.mesh_stack_available = true;
                telemetry.actor_render_instances = reply.alive_instances -
                    std::min(
                        reply.alive_instances,
                        reply.cluster_owned_instances);
                telemetry.actor_transitioning_instances =
                    reply.transitioning_instances;
                telemetry.actor_resource_bound_instances =
                    reply.resource_bound_instances;
                telemetry.transparent_actor_hard_cuts =
                    reply.transparent_hard_cuts;
                telemetry.mesh_vbo_segments = reply.vbo_segment_count;
                telemetry.mesh_ibo_segments = reply.ibo_segment_count;
                telemetry.mesh_vbo_growths = reply.vbo_growth_count;
                telemetry.mesh_ibo_growths = reply.ibo_growth_count;
                telemetry.mesh_vbo_used_bytes = reply.vbo_used_bytes;
                telemetry.mesh_vbo_free_bytes = reply.vbo_free_bytes;
                telemetry.mesh_vbo_largest_free_block =
                    reply.vbo_largest_free_block;
                telemetry.mesh_ibo_used_bytes = reply.ibo_used_bytes;
                telemetry.mesh_ibo_free_bytes = reply.ibo_free_bytes;
                telemetry.mesh_ibo_largest_free_block =
                    reply.ibo_largest_free_block;
                telemetry.mesh_vbo_fragmentation = reply.vbo_fragmentation;
                telemetry.mesh_ibo_fragmentation = reply.ibo_fragmentation;
            }
        }

        const auto light_ops = catalog.ops<
            lux::render::LightOperationIds>("Light");
        if (light_ops.valid())
        {
            lux::render::LightStatsReply reply{};
            if (impl_->settle(
                    lux::render::LightControlClient{
                        *impl_->control, light_ops}
                        .stats({scene}),
                    reply,
                    "LightStats"))
            {
                telemetry.light_render_available = true;
                telemetry.render_directional_lights =
                    reply.directional_lights;
                telemetry.render_point_lights = reply.point_lights;
                telemetry.render_spot_lights = reply.spot_lights;
                telemetry.render_area_lights = reply.area_lights;
                telemetry.transitioning_lights =
                    reply.transitioning_lights;
            }
        }

        const auto skybox_ops = catalog.ops<
            lux::render::SkyboxOperationIds>("Skybox");
        if (skybox_ops.valid())
        {
            lux::render::SkyboxStatsReply reply{};
            if (impl_->settle(
                    lux::render::SkyboxControlClient{
                        *impl_->control, skybox_ops}
                        .stats({scene}),
                    reply,
                    "SkyboxStats"))
            {
                telemetry.skybox_available = true;
                telemetry.skybox_active_mode = reply.active_mode;
                telemetry.skybox_bindless_index = reply.bindless_index;
                telemetry.skybox_pass_visits = reply.pass_visits;
                telemetry.skybox_draws = reply.draws;
                telemetry.skybox_inactive_pass_visits =
                    reply.inactive_pass_visits;
                telemetry.skybox_pipeline_bind_failures =
                    reply.pipeline_bind_failures;
                telemetry.skybox_intensity = reply.intensity;
            }
        }

        const auto terrain_ops = catalog.ops<
            lux::render::TerrainOperationIds>("Terrain");
        if (terrain_ops.valid())
        {
            lux::render::TerrainPageCacheStatsReply reply{};
            if (impl_->settle(
                    lux::render::TerrainControlClient{
                        *impl_->control, terrain_ops}
                        .stats({scene}),
                    reply,
                    "TerrainPageCacheStats"))
            {
                telemetry.terrain_available = true;
                telemetry.terrain_resident_pages = reply.resident_pages;
                telemetry.terrain_full_resolution_pages =
                    reply.full_resolution_pages;
                telemetry.terrain_fallback_pages = reply.fallback_pages;
                telemetry.terrain_selected_patches =
                    reply.selected_patch_count;
                telemetry.terrain_selected_patches_valid =
                    reply.selected_patch_count_valid != 0u;
                telemetry.terrain_fine_pages = reply.fine_pages;
                telemetry.terrain_hlod_pages = reply.hlod_pages;
                telemetry.terrain_drawable_pages = reply.drawable_pages;
                std::copy(
                    std::begin(reply.drawable_pages_by_level),
                    std::end(reply.drawable_pages_by_level),
                    std::begin(telemetry.terrain_drawable_pages_by_level));
                telemetry.terrain_transition_pages = reply.transition_pages;
                telemetry.terrain_view_surface_valid =
                    reply.debug_view_surface_valid != 0u;
                telemetry.terrain_view_surface_level =
                    reply.debug_view_surface_level;
                telemetry.terrain_view_page_x =
                    reply.debug_view_page_delta[0];
                telemetry.terrain_view_page_y =
                    reply.debug_view_page_delta[1];
                telemetry.terrain_view_page_z =
                    reply.debug_view_page_delta[2];
                telemetry.terrain_view_local_x = reply.debug_view_local[0];
                telemetry.terrain_view_local_y = reply.debug_view_local[1];
                telemetry.terrain_view_local_z = reply.debug_view_local[2];
                telemetry.terrain_view_surface_height =
                    reply.debug_view_surface_height;
                telemetry.terrain_view_surface_clearance =
                    reply.debug_view_surface_clearance;
                telemetry.terrain_cpu_bytes = reply.cpu_resident_bytes;
                telemetry.terrain_gpu_bytes = reply.gpu_resident_bytes;
            }
        }

        const auto water_ops = catalog.ops<
            lux::render::WaterOperationIds>("Water");
        if (water_ops.valid())
        {
            lux::render::WaterStatsReply reply{};
            if (impl_->settle(
                    lux::render::WaterControlClient{
                        *impl_->control, water_ops}
                        .stats({scene}),
                    reply,
                    "WaterStats"))
            {
                telemetry.water_available = true;
                telemetry.water_resident_surfaces =
                    reply.resident_surfaces;
                telemetry.water_visible_patches = reply.visible_patches;
                telemetry.water_transitioning_surfaces =
                    reply.transitioning_surfaces;
                telemetry.transparent_hard_cuts =
                    reply.transparent_hard_cuts;
                telemetry.water_cpu_bytes = reply.cpu_resident_bytes;
                telemetry.water_gpu_capacity_bytes =
                    reply.gpu_capacity_bytes;
            }
        }
        return telemetry;
    }

    std::optional<GameApplicationFrameCapture>
    GameApplication::captureMainView(std::uint32_t settle_frames)
    {
        if (!impl_ || !impl_->live || !impl_->runtime || !impl_->control ||
            !impl_->surface_target || impl_->surface_extent.width == 0u ||
            impl_->surface_extent.height == 0u)
        {
            return std::nullopt;
        }

        if (!impl_->diagnostic_capture_target)
        {
            lux::render::TargetReadyReply ready{};
            if (!impl_->settle(
                    impl_->control->createOffscreenRenderTarget(
                        impl_->surface_extent),
                    ready,
                    "create diagnostic capture target") ||
                ready.status != 0u || !ready.target.isValid())
            {
                return std::nullopt;
            }
            impl_->diagnostic_capture_target =
                impl_->control->adoptTarget(ready.target);
            impl_->diagnostic_capture_extent = impl_->surface_extent;
        }
        else if (impl_->diagnostic_capture_extent.width !=
                     impl_->surface_extent.width ||
                 impl_->diagnostic_capture_extent.height !=
                     impl_->surface_extent.height)
        {
            impl_->control->resizeTarget(
                impl_->diagnostic_capture_target.id(),
                impl_->surface_extent);
            impl_->diagnostic_capture_extent = impl_->surface_extent;
        }

        auto& capture_target = impl_->diagnostic_capture_target;
        auto* render_scene = lux::runtime::renderScene(*impl_->runtime);
        const auto restore_surface = [&]() noexcept
        {
            render_scene->reattachTarget(
                impl_->surface_target.id(),
                impl_->surface_extent);
            // Apply the ViewPresent observer and submit one restored surface
            // frame before allowing the temporary target to retire.
            return tick(0.0f, impl_->surface_extent);
        };
        render_scene->reattachTarget(
            capture_target.id(),
            impl_->surface_extent);
        const auto frames = std::max(1u, settle_frames);
        for (std::uint32_t frame = 0u; frame < frames; ++frame)
        {
            if (!tick(0.0f, impl_->surface_extent))
            {
                (void)restore_surface();
                return std::nullopt;
            }
        }

        GameApplicationFrameCapture result;
        result.extent = impl_->surface_extent;
        {
            result.render_graph_dump.resize(2u * 1024u * 1024u, '\0');
            lux::render::RenderGraphDumpReply graph_reply{};
            if (impl_->settle(
                    impl_->control->dumpRenderGraph(
                        render_scene->sceneId(),
                        result.render_graph_dump.data(),
                        result.render_graph_dump.size()),
                    graph_reply,
                    "dump diagnostic capture graph"))
            {
                result.render_graph_dump.resize(std::min<std::size_t>(
                    graph_reply.written,
                    result.render_graph_dump.size()));
            }
            else
            {
                result.render_graph_dump.clear();
            }
        }
        const auto pixel_count = static_cast<std::uint64_t>(
            result.extent.width) * result.extent.height;
        if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u)
        {
            (void)restore_surface();
            return std::nullopt;
        }
        result.pixels_bgra8.resize(
            static_cast<std::size_t>(pixel_count) * 4u);
        lux::render::ReadbackTargetReply readback{};
        const bool captured = impl_->settle(
            impl_->control->readbackTarget(
                capture_target.id(),
                result.pixels_bgra8.data(),
                result.pixels_bgra8.size()),
            readback,
            "read back diagnostic capture target");
        const bool restored = restore_surface();
        if (!captured || !restored || readback.status != 0u ||
            readback.width != result.extent.width ||
            readback.height != result.extent.height ||
            readback.bytes_per_pixel != 4u ||
            readback.bytes_written != result.pixels_bgra8.size())
        {
            return std::nullopt;
        }
        return result;
    }

    bool GameApplication::close() noexcept
    {
        if (!impl_)
            return true;
        const bool closed = impl_->close();
        if (closed)
            impl_.reset();
        return closed;
    }

    bool GameApplication::live() const noexcept
    {
        return impl_ && impl_->live;
    }

    bool GameApplication::surfaceAttached() const noexcept
    {
        return impl_ && static_cast<bool>(impl_->surface_target);
    }

    lux::input::Input& GameApplication::input() noexcept
    {
        return *impl_->input;
    }

    lux::extensions::EngineExtensions&
    GameApplication::extensions() noexcept
    {
        return *impl_->extensions;
    }
}
