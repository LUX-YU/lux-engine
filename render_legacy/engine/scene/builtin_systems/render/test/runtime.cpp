#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.type_static_info.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <lux/engine/ui/rendering/genops/UiRenderOperation.ops.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace std::chrono_literals;
    namespace ecs = simulation::ecs;

    std::atomic_bool backend_entered{}, backend_released{};

    render::RenderFeatureRegistration gateRegistration()
    {
        auto registration = render::kUiRenderRenderFeatureRegistration;
        auto& factory = registration.factory;
        factory.name = "TestGate";
        factory.descriptor.canonical_name = "test.render.gate";
        factory.descriptor.type = render::featureId(factory.descriptor.canonical_name);
        factory.operation_count = 0;
        factory.register_ops_fn = nullptr;
        factory.unregister_ops_fn = nullptr;
        factory.create_fn = +[](void*, const void*, std::size_t) -> render::Expected<render::FeatureHandle> {
            backend_entered.store(true, std::memory_order_release);
            while (!backend_released.load(std::memory_order_acquire))
                backend_released.wait(false);
            return render::renderFailure<render::err::comm::RequestInvalid>();
        };
        registration.scene_configurable = false;
        return registration;
    }

    struct Seed final
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.render.seed",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
    };

    scene::SceneSystemRegistration seedRegistration()
    {
        return {
            .type = system::systemTypeId(Seed::Description.canonical_name),
            .cpp_type = cxx::typeToken<Seed>(),
            .description = &Seed::Description,
            .install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure> {
                auto seed = installer.emplaceSystem<Seed>(description.instanceId());
                if (!seed)
                    return cxx::unexpected(seed.error());
                auto& registry = installer.registry();
                registry.emplace<scene::RenderViewRequest>(
                    registry.create(),
                    scene::RenderViewRequest{.system = {2}, .configuration = {.extent = {64, 48}}}
                );
                return {};
            }
        };
    }

    void verifyCodecs()
    {
        const auto codec = scene::renderSystemConfigurationCodec();
        const auto old_codec = serialization::makePortableValueCodec<scene::RenderSystemConfiguration>();
        scene::RenderSystemConfiguration configuration{.coordinate_page_size = 256};
        configuration.features.push_back({19, {std::byte{3}, std::byte{7}}, "test.feature", 4});
        std::vector<std::byte> old_bytes, bytes;
        assert(old_codec.encode(&configuration, old_bytes));
        assert(codec.encode(&configuration, bytes) && bytes == old_bytes);
        scene::RenderSystemConfiguration decoded;
        assert(old_codec.decode(bytes, &decoded));
        assert(
            decoded.coordinate_page_size == 256 &&
            decoded.features.front().configuration == configuration.features.front().configuration
        );
        bytes.pop_back();
        decoded.coordinate_page_size = 512;
        assert(!codec.decode(bytes, &decoded) && decoded.coordinate_page_size == 512);

        const auto& font_codec = render::kUiRenderRenderFeatureRegistration.configuration;
        assert(font_codec.valid());
        ui::FontAtlas font{std::vector<std::uint8_t>(256 * 256 * 4, 255), 256, 256};
        assert(font_codec.portable.encode(&font, bytes) && bytes.size() > 65536);
        ui::FontAtlas restored;
        assert(font_codec.portable.decode(bytes, &restored));
        assert(restored.width == 256 && restored.height == 256 && restored.pixels == font.pixels);
        std::vector<std::byte> wire;
        assert(font_codec.materialize_attach(bytes, wire));
        assert(wire.size() == sizeof(render::UiRenderCommConfig) + font.pixels.size());
        const auto previous = wire;
        bytes.push_back(std::byte{0});
        assert(!font_codec.materialize_attach(bytes, wire) && wire == previous);
        assert(!font_codec.portable.decode(bytes, &restored) && restored.pixels == font.pixels);
        bytes.resize(16);
        std::fill(bytes.begin() + 8, bytes.end(), std::byte{255});
        assert(!font_codec.portable.decode(bytes, &restored)); // Reject length before allocation.
        font.width = -1;
        assert(!font_codec.portable.encode(&font, wire) && wire == previous);
        assert(!font_codec.portable.encode_default(wire) && wire == previous);
        std::cout << "PASS bounded font codec, malformed input strong guarantee, existing scene configuration bytes\n";
    }
}

void verifyRenderContext()
{
    std::cout << std::unitbuf;
    verifyCodecs();
    scene::RenderViewReceipt retired;
    const auto started = std::chrono::steady_clock::now();
    {
        auto created = engine::EngineContext::create({1, 64, 64, {64}}, {0, 1024});
        assert(created);
        auto& engine = **created;
        assert(!engine.renderContext());
        assert(engine::initializeRendering(engine, {}));
        auto* rendering = engine.renderContext();
        assert(!engine::initializeRendering(engine, {}) && engine.renderContext() == rendering);
        const auto& ui_feature = render::kUiRenderRenderFeatureRegistration;
        assert(rendering->registerFeatures({ui_feature, gateRegistration()}));
        assert(!rendering->registerFeatures({ui_feature, ui_feature}));
        assert(rendering->runtime().features().find(ui_feature.factory.descriptor.type));

        auto& resources = rendering->resources();
        auto& scenes = engine.sceneRuntime();
        const auto render_registration = scene::builtinRenderSystemRegistration();
        const std::array registrations{seedRegistration(), render_registration};
        const ecs::ComponentSchemaSet components;
        const simulation::SimulationSystemRegistry simulation_systems;
        scene::RenderFeatureSceneBindings bindings;
        const std::array providers{
            scene::makeSceneCapabilityProvider<render::RenderRuntime>(
                "runtime",
                "lux.render.runtime",
                rendering->runtime()
            ),
            scene::makeSceneCapabilityProvider<scene::RenderResources>("resources", "lux.render.resources", resources),
            scene::makeSceneCapabilityProvider<scene::RenderFeatureSceneBindings>(
                "bindings",
                "lux.render.scene_bindings",
                bindings
            )
        };
        ui::FontAtlas font{std::vector<std::uint8_t>(256 * 256 * 4, 255), 256, 256};
        std::vector<std::byte> pixels, configuration;
        assert(ui_feature.configuration.portable.encode(&font, pixels));
        scene::RenderSystemConfiguration render_configuration;
        render_configuration.features.push_back(
            {ui_feature.factory.descriptor.type,
             pixels,
             std::string(ui_feature.configuration.schema),
             ui_feature.configuration.schema_version}
        );
        assert(render_registration.configuration.encode(&render_configuration, configuration));
        const auto make_description = [&](bool duplicate) {
            scene::SceneDescriptionBuilder description;
            assert(description.addSystem({1}, "seed", registrations[0].type, 1, {}, 0));
            for (std::uint64_t id = 2; id <= (duplicate ? 3u : 2u); ++id)
                assert(description.addSystem(
                    {id},
                    id == 2 ? "render" : "duplicate-render",
                    render_registration.type,
                    render_registration.description->version,
                    render_registration.description->configuration_schema_name,
                    render_registration.description->configuration_schema_version,
                    configuration
                ));
            assert(description.addDependency({1}, {2}));
            auto resolved = std::move(description).buildResolved();
            assert(resolved);
            return std::make_shared<const scene::SceneDescription>(std::move(*resolved));
        };
        auto builder = scenes.builder();
        builder.setDescription(make_description(true))
            .setWorld(std::make_shared<const world::WorldDescription>())
            .setSimulation(std::make_shared<const simulation::SimulationDescription>())
            .setRegistrations(components, simulation_systems, registrations)
            .setProviders(providers);
        const auto duplicate = builder.build();
        assert(!duplicate);
        const auto* failure = std::get_if<scene::SceneBuildFailure>(&duplicate.error().cause);
        assert(failure && failure->scene_system.code == scene::ESceneSystemBuildError::DUPLICATE_SYSTEM);
        builder.setDescription(make_description(false));
        const auto built = builder.build();
        assert(built);
        const auto id = built->id();
        assert(scenes.pauseSimulation(id));
        {
            const auto& registry = std::as_const(scenes).borrowInstance(id)->get();
            const auto* first = scene::RenderSceneState::find(registry, {2});
            assert(first && first->coordinate_page_size == render_configuration.coordinate_page_size);
            assert(!scene::RenderSceneState::find(registry, {3}));
            assert(!scene::RenderSceneState::find(registry, {999}));
        }
        const auto until = [&](auto predicate, std::source_location call = std::source_location::current()) {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    std::cerr << "timeout at " << call.line() << '\n';
                    auto registry = std::as_const(scenes).borrowInstance(id);
                    for (const auto entity : registry->get().view<scene::RenderViewResult>())
                    {
                        const auto& result = registry->get().get<scene::RenderViewResult>(entity);
                        const auto observed = resources.observeView(result.view);
                        std::cerr << "view " << entt::to_integral(entity) << " adopted=" << result.adopted_revision
                                  << " published=" << result.published_revision
                                  << " sequence=" << result.published_sequence << " error=" << bool(result.failure);
                        if (observed)
                            std::cerr << " state=" << int(observed->status.state)
                                      << " producer=" << observed->render_sequence
                                      << " requested=" << observed->status.request_sequence;
                        std::cerr << '\n';
                    }
                    assert(false && "request completion deadline");
                }
                assert(engine.execution().collectCompletions());
                auto tick = scenes.driveFrame();
                assert(tick && tick->empty());
                std::this_thread::sleep_for(1ms);
            }
        };
        until([&] { return static_cast<bool>(scenes.borrowInstance(id)); });
        ecs::Entity entity;
        {
            auto registry = scenes.borrowInstance(id);
            entity = registry->get().view<scene::RenderViewRequest>().front();
        }
        const auto published = [&](std::uint64_t revision) {
            auto registry = scenes.borrowInstance(id);
            if (!registry)
                return false;
            const auto* result = registry->get().try_get<scene::RenderViewResult>(entity);
            return result && result->published_revision == revision && !result->failure;
        };
        until([&] { return published(1); }
        ); // Pre-existing request, actual font attach and no camera/asset source/loader.
        scene::RenderResourceId original;
        {
            auto registry = scenes.borrowInstance(id);
            const auto& result = registry->get().get<scene::RenderViewResult>(entity);
            original = result.view;
            assert(resources.observeView(original)->status.state == scene::EViewState::READY);
            registry->get().patch<scene::RenderViewRequest>(entity, [](auto& request) {
                request.configuration.extent = {80, 60};
                request.revision = 2;
            });
            registry->get().patch<scene::RenderViewRequest>(entity, [](auto& request) {
                request.configuration.extent = {96, 72};
                request.revision = 3;
            });
        }
        until([&] {
            const auto view = resources.observeView(original);
            return published(3) && view && view->render_extent == render::PixelExtent{96, 72} &&
                   view->render_sequence == view->status.request_sequence;
        });
        const auto clock = scenes.borrowClock(id);
        assert(clock && std::visit([](const auto& value) { return value.snapshot().step_index; }, clock->get()) == 0);

        // Malformed requests fail locally and cannot stop another request or the scene.
        ecs::Entity malformed;
        {
            auto registry = scenes.borrowInstance(id);
            malformed = registry->get().create();
            registry->get().emplace<scene::RenderViewRequest>(
                malformed,
                scene::RenderViewRequest{.system = {999}, .configuration = {.extent = {32, 32}}}
            );
            registry->get().patch<scene::RenderViewRequest>(entity, [](auto& request) {
                request.configuration.extent = {128, 96}; // Same revision is rejected.
            });
        }
        until([&] {
            auto registry = scenes.borrowInstance(id);
            if (!registry)
                return false;
            auto* result = registry->get().try_get<scene::RenderViewResult>(malformed);
            auto* stale = registry->get().try_get<scene::RenderViewResult>(entity);
            return result && result->failure && stale && stale->failure;
        });
        assert(resources.observeView(original));
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().patch<scene::RenderViewRequest>(entity, [](auto& request) { request.revision = 4; });
        }
        until([&] { return published(4); });
        ecs::Entity camera;
        {
            auto registry = scenes.borrowInstance(id);
            camera = registry->get().create();
            registry->get().emplace<scene::Camera>(camera);
            registry->get().patch<scene::RenderViewRequest>(entity, [&](auto& request) {
                request.camera = camera;
                request.revision = 5;
            });
        }
        until([&] { return published(5); });
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().destroy(camera);
        }
        until([&] {
            auto registry = scenes.borrowInstance(id);
            return registry && registry->get().get<scene::RenderViewResult>(entity).failure.has_value();
        });
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().patch<scene::RenderViewRequest>(entity, [&](auto& request) {
                request.camera = ecs::NullEntity;
                request.configuration.extent = {};
                request.revision = 6;
            });
        }
        until([&] { return resources.observeView(original)->status.state == scene::EViewState::SUSPENDED; });
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().patch<scene::RenderViewRequest>(entity, [&](auto& request) {
                request.configuration.extent = {64, 64};
                request.revision = 7;
            });
        }
        until([&] { return published(7); });
        // Cancellation revokes a view owner without requiring another scene traversal.
        std::stop_source view_owner;
        ecs::Entity cancellable;
        {
            auto registry = scenes.borrowInstance(id);
            assert(scene::RenderAssets::find(registry->get(), {2}));
            assert(!scene::RenderAssets::find(registry->get(), {999}));
            cancellable = registry->get().create();
            registry->get().emplace<scene::RenderViewRequest>(
                cancellable,
                scene::RenderViewRequest{
                    .system = {2},
                    .configuration = {.extent = {16, 16}},
                    .stop = view_owner.get_token()
                }
            );
        }
        scene::RenderResourceId cancelled_view;
        until([&] {
            auto registry = scenes.borrowInstance(id);
            if (!registry)
                return false;
            const auto* result = registry->get().try_get<scene::RenderViewResult>(cancellable);
            if (!result || !result->published_revision)
                return false;
            cancelled_view = result->view;
            return true;
        });
        const auto cancelled_receipt = *resources.viewReceipt(cancelled_view);
        view_owner.request_stop();
        const auto cancel_deadline = std::chrono::steady_clock::now() + 15s;
        while (cancelled_receipt.status().status.state != scene::EViewState::CLOSED)
        {
            assert(std::chrono::steady_clock::now() < cancel_deadline);
            assert(engine.execution().collectCompletions());
            std::this_thread::sleep_for(1ms);
        }
        assert(!resources.retain(cancelled_view));
        until([&] {
            const auto registry = std::as_const(scenes).borrowInstance(id);
            return !registry->get().all_of<scene::RenderViewRequest>(cancellable);
        });
        assert(std::as_const(scenes).borrowInstance(id)->get().valid(cancellable));
        std::cout << "PASS request cancellation retires a view using completions only\n";
        // Dedicated requests opt into entity cleanup; a caller's ordinary entity survives.
        for (const bool adopt_first : {false, true})
        {
            std::stop_source owner;
            ecs::Entity transient;
            {
                auto registry = scenes.borrowInstance(id);
                transient = registry->get().create();
                registry->get().emplace<scene::RenderViewRequest>(
                    transient,
                    scene::RenderViewRequest{
                        .system = {2},
                        .configuration = {.extent = {16, 16}},
                        .stop = owner.get_token(),
                        .destroy_entity_on_stop = true
                    }
                );
            }
            if (adopt_first)
                until([&] {
                    const auto registry = std::as_const(scenes).borrowInstance(id);
                    const auto* result = registry->get().try_get<scene::RenderViewResult>(transient);
                    return result && result->published_revision != 0;
                });
            owner.request_stop();
            // Cancellation itself must never mutate an EnTT pool.
            assert(std::as_const(scenes).borrowInstance(id)->get().valid(transient));
            until([&] { return !std::as_const(scenes).borrowInstance(id)->get().valid(transient); });
        }
        std::cout << "PASS transient request entities retire at maintenance, before or after adoption\n";
        // Block a real backend create callback, then fill the actual Program queue.
        // No zero budget, production test switch or timing-dependent GPU load creates this wait.
        const auto observation = resources.observeView(original);
        assert(observation);
        auto blocked = rendering->runtime().control()->get().addFeatureRaw(
            observation->scene,
            rendering->runtime().features().typeId("TestGate"),
            std::span<const std::byte>{}
        );
        until([&] { return backend_entered.load(std::memory_order_acquire); });
        render::TRenderProgram<> filler;
        bool backpressured{};
        for (unsigned index{}; index != 32; ++index)
        {
            render::RenderProgramSession::Builder packet(filler);
            packet.begin({});
            filler.kind = render::ERenderProgramKind::STATE_UPDATE;
            const auto result = rendering->runtime().submit(filler);
            assert(result);
            if (*result == render::EFrameSubmit::BACKPRESSURED)
            {
                backpressured = true;
                break;
            }
        }
        assert(backpressured);
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().patch<scene::RenderViewRequest>(entity, [](auto& request) { request.revision = 8; });
        }
        for (unsigned retry{}; retry != 3; ++retry)
        {
            const auto tick = scenes.driveFrame();
            assert(tick && tick->empty());
            assert(!scenes.borrowInstance(id)); // No writable borrow while the packet occupies this instance.
            auto registry = std::as_const(scenes).borrowInstance(id);
            const auto& result = registry->get().get<scene::RenderViewResult>(entity);
            assert(result.adopted_revision == 8 && result.published_revision == 0);
        }
        backend_released.store(true, std::memory_order_release);
        backend_released.notify_all();
        until([&] { return published(8) && blocked.isReady(); });
        assert(blocked.tryResult() && !blocked.tryResult()->get().error.ok());
        std::cout << "PASS real Program backpressure: three retries keep unpublished request, then ordered adoption\n";
        const auto stable_start = std::chrono::steady_clock::now();
        constexpr unsigned StableIterations = 1024;
        for (unsigned iteration{}; iteration != StableIterations; ++iteration)
        {
            auto tick = scenes.driveFrame();
            assert(tick && tick->empty());
        }
        const auto stable_time = std::chrono::steady_clock::now() - stable_start;
        std::cout << "stable paused request tick mean ns="
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(stable_time).count() / StableIterations
                  << '\n';
        retired = *resources.viewReceipt(original);
        // Remove and reconstruct at the same Entity before maintenance. The old reference must still be released.
        {
            auto registry = scenes.borrowInstance(id);
            registry->get().remove<scene::RenderViewRequest>(entity);
            registry->get().emplace<scene::RenderViewRequest>(
                entity,
                scene::RenderViewRequest{.system = {2}, .configuration = {.extent = {48, 48}}}
            );
        }
        until([&] {
            auto registry = scenes.borrowInstance(id);
            if (!registry)
                return false;
            const auto* result = registry->get().try_get<scene::RenderViewResult>(entity);
            return result && result->view != original && result->published_revision == 1;
        });
        until([&] { return retired.status().status.state == scene::EViewState::CLOSED; });
        assert(!resources.retain(original));
        {
            auto registry = scenes.borrowInstance(id);
            retired = *resources.viewReceipt(registry->get().get<scene::RenderViewResult>(entity).view);
        }
        // No explicit Scene destroy, resource close, event loop or Runtime polling after this point.
        // EngineContext destroys SceneRuntime before rendering; its completion-only RAII wait retires the views.
    }
    assert(retired.status().status.state == scene::EViewState::CLOSED);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::cout << "PASS RenderContext / Runtime request adoption, revisions, local failure, paused clock, RAII: "
              << elapsed.count() << " ms\n";
}
