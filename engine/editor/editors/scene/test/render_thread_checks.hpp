#pragma once

#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.ops.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/render/RenderView.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneDriver.hpp>
#include <lux/engine/scene/SceneInstance.hpp>

#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <thread>

// Deterministic transport experiment, not a GPU timing result. The consumer
// advances through StateUpdates up to one Frame per turn, as drainTick does.
// Hold the ring full initially, then offer a retained update alongside the UI.
inline void checkProgramAdmissionOrder()
{
    using namespace lux::render;
    for (std::size_t capacity = 1; capacity <= 3; ++capacity)
    {
        for (const unsigned policy : {0U, 1U, 2U})
        {
            auto channel = RenderProgramChannel<>::create(capacity);
            auto sync = std::make_shared<RenderChannelSync>();
            RenderProgramSession session(channel, sync);
            RenderProgram<> frame;
            RenderProgram<> update;
            update.payload.push_back(std::byte{1});
            std::size_t adopted_updates{}, adopted_frames{}, accepted_updates{};
            std::size_t allowance = 64;

            const auto offer_frame = [&] {
                if (allowance && session.hasPendingSubmit() && session.retryPendingSubmit())
                {
                    --allowance;
                }
                frame.kind = ERenderProgramKind::Frame;
                if (allowance && session.trySubmitPrepared(frame))
                {
                    --allowance;
                }
            };
            const auto offer_update = [&] {
                if (!allowance || accepted_updates == 64)
                {
                    return;
                }
                if (session.hasPendingSubmit())
                {
                    if (!session.retryPendingSubmit())
                    {
                        return;
                    }
                    --allowance;
                    if (!allowance)
                    {
                        return;
                    }
                }
                if (session.trySubmitPrepared(update))
                {
                    ++accepted_updates;
                    --allowance;
                    update.payload.push_back(std::byte(accepted_updates + 1));
                }
                else
                {
                    assert(update.payload.size() == 1);
                    assert(update.payload.front() == std::byte(accepted_updates + 1));
                }
            };
            for (std::size_t i{}; i < RenderProgramChannel<>::request_slot_count; ++i)
            {
                offer_frame();
            }
            for (std::size_t turn{}; turn < 256; ++turn)
            {
                while (channel->requests.tryAcquireRead())
                {
                    const auto &program = channel->requests.currentRead();
                    if (program.kind == ERenderProgramKind::Frame)
                    {
                        ++adopted_frames;
                        break;
                    }
                    ++adopted_updates;
                    assert(program.payload.size() == 1);
                    assert(program.payload.front() == std::byte(adopted_updates));
                }
                allowance = policy == 2 ? 1 : 2;
                if (policy == 1 || (policy == 2 && turn % 2 == 0))
                {
                    offer_update();
                    offer_frame();
                }
                else
                {
                    offer_frame();
                    offer_update();
                }
            }
            std::printf("Program contention capacity=%zu policy=%u accepted_updates=%zu "
                        "adopted_updates=%zu adopted_frames=%zu\n",
                        capacity, policy, accepted_updates, adopted_updates, adopted_frames);
            assert(adopted_frames > 0);
            assert(adopted_updates == (policy ? 64 : 0));
            assert(accepted_updates == adopted_updates);
        }
    }
}

struct RenderThreadChecks final
{
    std::shared_ptr<const lux::scene::RenderSystemMetadata> metadata;
    lux::scene::SceneDescription description;
    std::unique_ptr<lux::scene::SceneInstance> instance;
    lux::scene::RenderSystem *system{};
    lux::render::RenderSceneReceipt receipt;
    lux::task::TaskExecutor executor{*lux::task::TaskExecutor::create({0, 1024})};
    lux::scene::SceneDriver driver{executor};
    lux::simulation::ecs::Entity entity;
    std::unique_ptr<lux::render::RenderView> view;
    std::size_t phase{}, frames{};
    std::uint64_t completed_before{};

    ~RenderThreadChecks()
    {
        assert(!instance);
    }

    void begin(lux::render::RenderRuntime &renderer)
    {
        using namespace lux;
        std::vector<simulation::ecs::ComponentSchema> schemas;
        const auto append = [&](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
        append(simulation::ecs::transformComponentSchemas());
        append(simulation::ecs::visualComponentSchemas());
        append(scene::sceneRenderComponentSchemas());
        auto components = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
        assert(components);
        const auto systems = scene::builtinRenderSystemRegistrations();
        const auto features = render::builtinRenderFeatureRegistrations();
        const auto bindings = scene::builtinRenderFeatureSceneBindings();
        auto built = scene::SceneMetaManager::build({std::move(*components), {}, {systems.begin(), systems.end()}});
        assert(built);
        auto render_meta = scene::RenderSystemMetadata::build(*built, {features.begin(), features.end()}, bindings);
        assert(render_meta);
        metadata = std::make_shared<const scene::RenderSystemMetadata>(std::move(*render_meta));
        const auto light = std::ranges::find(features, std::string_view("lux.render.light.v1"),
                                             &render::RenderFeatureRegistration::stable_name);
        assert(light != features.end());
        std::vector<std::byte> defaults;
        assert(light->configuration.portable.encode_default(defaults));
        scene::RenderSystemConfiguration config;
        config.coordinate_page_size = 2048;
        config.features.push_back({light->descriptor->type, std::move(defaults)});
        const auto registration = scene::builtinRenderSystemRegistration();
        std::vector<std::byte> encoded;
        assert(registration.configuration.encode(&config, encoded));
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        scene::SceneDescriptionBuilder builder;
        builder.setWorld(asset::AssetId{uuids::uuid{bytes}});
        bytes.back() = 2;
        builder.setSimulation(asset::AssetId{uuids::uuid{bytes}});
        assert(builder.addSystem(system::SystemInstanceId{1}, "thread-regression", registration.type,
                                 scene::RenderSystem::Description.version,
                                 scene::RenderSystem::Description.configuration_schema_name,
                                 scene::RenderSystem::Description.configuration_schema_version, encoded));
        auto assembled = std::move(builder).build();
        assert(assembled);
        description = std::move(*assembled);
        std::array providers{
            scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", renderer),
            scene::makeSceneCapabilityProvider<std::shared_ptr<const scene::RenderSystemMetadata>>(
                "metadata", "lux.render.metadata", metadata)};
        auto made =
            scene::SceneInstance::create({std::make_shared<const scene::SceneDescription>(std::move(description)),
                                          std::make_shared<const world::WorldDescription>(),
                                          std::make_shared<const simulation::SimulationDescription>(), *built,
                                          providers, simulation::ESimulationMode::DERIVATION});
        assert(made && (*made)->simulation().seal());
        instance = std::move(*made);
        system = instance->findSceneSystem<scene::RenderSystem>();
        assert(system);
        receipt = system->resourceReceipt();
    }

    bool poll(lux::process::ExecutionRuntime &, lux::render::RenderRuntime &renderer)
    {
        using namespace lux;
        if (phase == 0)
        {
            const auto status = receipt.status();
            assert(status.failure.ok());
            if (status.state != render::ESceneResourceState::READY)
            {
                return false;
            }
            std::thread wrong_thread([&] {
                const auto wrong = renderer.control();
                assert(!wrong && wrong.error().code == render::ERendererError::WRONG_THREAD);
            });
            wrong_thread.join();
            auto opened = system->openView({.extent = {320, 240}});
            assert(opened);
            view = std::move(*opened);
            auto &registry = instance->registry();
            entity = registry.create();
            registry.emplace<simulation::ecs::WorldTransform3D>(entity);
            registry.emplace<simulation::ecs::Light3D>(entity);
            scene::SceneAdvanceBudget budget{32, 1, 0};
            assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) ==
                   scene::ESceneProgress::PENDING);
            assert(system->transportStatistics().published == 1 && system->transportStatistics().pending == 1);
            registry.patch<simulation::ecs::Light3D>(entity, [](auto &light) { light.value.intensity = 2; });
            completed_before = renderer.statistics().gpu_completed;
            phase = 1;
        }
        else if (phase == 1)
        {
            scene::SceneAdvanceBudget budget{32, 1, 0};
            assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) ==
                   scene::ESceneProgress::PENDING);
            const auto counts = system->transportStatistics();
            assert(counts.published == 1 && counts.pending == 1 && counts.forwarded == 0);
            if (++frames < 12 || renderer.statistics().gpu_completed <= completed_before)
            {
                return false;
            }
            phase = 2;
        }
        else if (phase == 2)
        {
            const auto before = system->transportStatistics().forwarded;
            scene::SceneAdvanceBudget budget{32, 1, 1};
            static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
            assert(instance->progress().result);
            const auto counts = system->transportStatistics();
            assert(counts.forwarded - before <= 1);
            if (counts.forwarded != 2)
            {
                return false;
            }
            assert(counts.published == 2 && counts.pending == 0);
            assert(view->beginClose());
            phase = 3;
        }
        else if (phase == 3)
        {
            auto closed = view->advanceClose();
            assert(closed);
            if (*closed != render::ERenderClose::COMPLETE)
            {
                return false;
            }
            view.reset();
            instance.reset();
            system = nullptr;
            phase = 4;
        }
        else if (phase == 4)
        {
            const auto status = receipt.status();
            assert(status.failure.ok());
            if (status.state != render::ESceneResourceState::RETIRED)
            {
                return false;
            }
            std::puts(
                "PASS Main SceneDriver: thread guard, retained publication, budget 0/1, GPU progress, RAII retirement");
            return true;
        }
        return false;
    }
};
