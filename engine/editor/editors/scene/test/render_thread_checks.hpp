#pragma once

#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/rendering/RenderView.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>
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
        for (const bool documents_first : {false, true})
        {
            auto channel = RenderProgramChannel<>::create(capacity);
            auto sync = std::make_shared<RenderChannelSync>();
            RenderProgramSession session(channel, sync);
            RenderProgram<> frame;
            RenderProgram<> update;
            update.payload.push_back(std::byte{1});
            std::size_t adopted_updates{}, adopted_frames{}, accepted_updates{};

            const auto offer_frame = [&]
            {
                static_cast<void>(session.retryPendingSubmit());
                frame.kind = ERenderProgramKind::Frame;
                static_cast<void>(session.trySubmitPrepared(frame));
            };
            const auto offer_update = [&]
            {
                if (accepted_updates == 64)
                {
                    return;
                }
                if (session.hasPendingSubmit() && !session.retryPendingSubmit())
                {
                    return;
                }
                if (session.trySubmitPrepared(update))
                {
                    ++accepted_updates;
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
                if (documents_first)
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
            std::printf("Program contention capacity=%zu documents_first=%u accepted_updates=%zu "
                        "adopted_updates=%zu adopted_frames=%zu\n",
                        capacity, documents_first, accepted_updates, adopted_updates, adopted_frames);
            assert(adopted_frames > 0);
            assert(adopted_updates == (documents_first ? 64 : 0));
            assert(accepted_updates == adopted_updates);
        }
    }
}

struct RenderThreadChecks final
{
    std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
    lux::scene::SceneDescription description;
    std::unique_ptr<lux::scene::SceneRenderBinding> binding;
    lux::process::TaskScope task;
    std::stop_source stop;
    std::atomic_uint milestone{};
    std::atomic_bool done{};
    std::atomic_bool working{}, finish_work{};
    std::atomic_uint64_t work_checksum{};
    std::unique_ptr<lux::editor::rendering::RenderView> view;
    std::size_t phase{}, frames{};
    std::uint64_t completed_before{};
    std::chrono::steady_clock::time_point stopping_at;
    std::thread::id main_thread{std::this_thread::get_id()};

    ~RenderThreadChecks()
    {
        stop.request_stop();
        assert(stdexec::sync_wait(task.close()));
        assert(!binding);
    }

    void begin(lux::editor::rendering::EditorRenderer &renderer)
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
        auto built = scene::SceneMetaManager::build({std::move(*components),
                                                     {},
                                                     {systems.begin(), systems.end()},
                                                     {features.begin(), features.end()},
                                                     {bindings.begin(), bindings.end()}});
        assert(built);
        metadata = std::make_shared<const scene::SceneMetaManager>(std::move(*built));
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
        auto begun = scene::SceneRenderBinding::begin(renderer, description.systemAt(0), metadata);
        assert(begun);
        binding = std::move(*begun);
    }
    bool poll(lux::process::ExecutionRuntime &runtime, lux::editor::rendering::EditorRenderer &renderer)
    {
        using namespace lux;
        if (phase == 0)
        {
            assert(binding->poll(0) == 0);
            assert(binding->state() != scene::ESceneRenderBindingState::FAILED);
            if (binding->state() != scene::ESceneRenderBindingState::READY)
            {
                return false;
            }
            auto input = binding->takeInput();
            assert(input);
            auto opened = renderer.openView(input->sceneId(), {{320, 240}, true, 2048});
            assert(opened);
            view = std::move(*opened);
            auto work = stdexec::then(
                stdexec::schedule(runtime.cpu()),
                [this, &renderer, input = std::move(*input)]() mutable noexcept
                {
                    assert(std::this_thread::get_id() != main_thread);
                    const auto wrong = renderer.acquire();
                    assert(!wrong && wrong.error().code == scene::ERenderRuntimeError::ACTIVATION_FAILURE);
                    simulation::ecs::Registry registry;
                    auto pipeline = input.makePipeline(registry, description.systemAt(0));
                    assert(pipeline);
                    const auto entity = registry.create();
                    registry.emplace<simulation::ecs::WorldTransform3D>(entity);
                    registry.emplace<simulation::ecs::Light3D>(entity);
                    working.store(true, std::memory_order_release);
                    std::uint64_t checksum{1};
                    while (!finish_work.load(std::memory_order_acquire) && !stop.stop_requested())
                    {
                        for (std::size_t i = 0; i < 4096; ++i)
                        {
                            checksum = checksum * 6364136223846793005ULL + i + 1;
                        }
                        work_checksum.store(checksum, std::memory_order_relaxed);
                    }
                    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::FULL_SYNC_PUBLISHED);
                    registry.patch<simulation::ecs::Light3D>(entity, [](auto &light) { light.value.intensity = 2; });
                    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::BACKPRESSURED);
                    milestone.store(1, std::memory_order_release);
                    assert((*pipeline)->waitForCapacity(stop.get_token()));
                    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::PUBLISHED);
                    registry.patch<simulation::ecs::Light3D>(entity, [](auto &light) { light.value.intensity = 3; });
                    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::BACKPRESSURED);
                    milestone.store(2, std::memory_order_release);
                    assert(!(*pipeline)->waitForCapacity(stop.get_token()));
                    pipeline->reset(); // Stages and their Registry observations retire on the worker.
                    done.store(true, std::memory_order_release);
                });
            auto errors =
                stdexec::upon_error(std::move(work), [](process::EExecutionError) noexcept { assert(false); });
            auto stopped = stdexec::upon_stopped(std::move(errors), []() noexcept { assert(false); });
            assert(task.start(std::move(stopped)));
            phase = 10;
        }
        else if (phase == 10 && working.load(std::memory_order_acquire))
        {
            completed_before = renderer.statistics().gpu_completed;
            frames = 0;
            phase = 11;
        }
        else if (phase == 11)
        {
            editor::rendering::CameraFrame camera;
            camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            camera.projection = camera.view;
            camera.origin[0] = double(frames) / 10;
            camera.desired = {1, 0, ++frames, 1};
            assert(view->setCamera(camera));
            assert(binding->poll(0) == 0 && binding->statistics().published == 0);
            if (frames < 20 || renderer.statistics().gpu_completed <= completed_before)
            {
                return false;
            }
            assert(work_checksum.load(std::memory_order_relaxed) != 0);
            std::printf("T02: worker CPU work pending; Main camera_updates=%zu GPU_completed_delta=%llu\n", frames,
                        static_cast<unsigned long long>(renderer.statistics().gpu_completed - completed_before));
            finish_work.store(true, std::memory_order_release);
            phase = 1;
        }
        else if (phase == 1 && milestone.load(std::memory_order_acquire) == 1)
        {
            completed_before = renderer.statistics().gpu_completed;
            frames = 0;
            phase = 2;
        }
        else if (phase == 2)
        {
            assert(binding->poll(0) == 0);
            const auto counts = binding->statistics();
            assert(counts.published == 1 && counts.forwarded == 0 && counts.pending == 1);
            if (++frames < 12 || renderer.statistics().gpu_completed <= completed_before)
            {
                return false;
            }
            assert(milestone.load(std::memory_order_acquire) == 1);
            phase = 22;
        }
        else if (phase == 22)
        {
            const auto forwarded = binding->poll(1);
            assert(forwarded <= 1);
            if (!forwarded)
            {
                return false;
            }
            assert(binding->statistics().forwarded == 1);
            phase = 3;
        }
        else if (phase == 3 && milestone.load(std::memory_order_acquire) == 2)
        {
            assert(binding->poll(0) == 0);
            const auto counts = binding->statistics();
            assert(counts.published == 2 && counts.forwarded == 1 && counts.pending == 1 && counts.high_water == 1);
            stopping_at = std::chrono::steady_clock::now();
            stop.request_stop();
            phase = 4;
        }
        else if (phase == 4 && done.load(std::memory_order_acquire))
        {
            assert(stdexec::sync_wait(task.close()));
            // Stop does not pretend the already accepted second packet never existed.
            assert(binding->statistics().pending == 1);
            phase = 44;
        }
        else if (phase == 44)
        {
            const auto forwarded = binding->poll(1);
            assert(forwarded <= 1);
            if (!forwarded)
            {
                return false;
            }
            assert(binding->statistics().forwarded == 2);
            assert(binding->poll(4) == 0);
            phase = 45;
        }
        else if (phase == 45)
        {
            assert(view->beginClose());
            const auto closed = view->advanceClose();
            assert(closed);
            if (*closed != editor::rendering::ERenderClose::COMPLETE)
            {
                return false;
            }
            view.reset();
            binding->requestClose();
            phase = 5;
        }
        else if (phase == 5)
        {
            assert(binding->poll(4) <= 4);
            if (binding->state() != scene::ESceneRenderBindingState::CLOSED)
            {
                return false;
            }
            const auto counts = binding->statistics();
            assert(counts.published == counts.forwarded && counts.pending == 0);
            std::printf("PASS T01/T02/T03/T04/T05/T06: Main guard preserved, worker-only producer, "
                        "GPU progresses while blocked; budget 0/1/4; published=%llu forwarded=%llu high_water=%u "
                        "backpressure=%llu Stop_close_us=%lld (forwarded is not GPU completion)\n",
                        static_cast<unsigned long long>(counts.published),
                        static_cast<unsigned long long>(counts.forwarded), counts.high_water,
                        static_cast<unsigned long long>(counts.backpressured),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   std::chrono::steady_clock::now() - stopping_at)
                                                   .count()));
            binding.reset();
            return true;
        }
        return false;
    }
};
