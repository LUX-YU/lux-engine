#include <lux/cxx/core/scope_exit.hpp>
#include "api_contract.hpp"
#include "support/PreparedProjectFixture.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;

namespace
{
    struct CloseProbe final : object::LuxObject
    {
        execution::ExecutionRuntime& execution;
        unsigned first = 0, second = 0, independent = 0, closing = 0, closed = 0;
        bool fail = true;

        explicit CloseProbe(execution::ExecutionRuntime& value) : execution(value) {}

        void onClosing(scene::SceneInstanceId) noexcept
        {
            ++closing;
        }

        void onClosed(scene::SceneInstanceId) noexcept
        {
            ++closed;
        }

        exec::task<scene::SceneCloseResult> finishFirst() noexcept
        {
            ++first;
            co_await stdexec::schedule(execution.mainScheduler());
            co_return scene::SceneCloseResult{};
        }

        exec::task<scene::SceneCloseResult> finishIndependent() noexcept
        {
            ++independent;
            co_return scene::SceneCloseResult{};
        }

        exec::task<scene::SceneCloseResult> finishSecond() noexcept
        {
            assert(first == 1);
            ++second;
            if (fail)
            {
                co_return cxx::unexpected(error::Error{error::errorId("test.editor.close.failure")});
            }
            co_return scene::SceneCloseResult{};
        }
    };

    void checkSceneClose(engine::Engine& engine)
    {
        auto& runtime = engine.sceneRuntime();
        scene::SceneDescriptionBuilder description;
        auto resolved = std::move(description).buildResolved();
        assert(resolved);
        ecs::ComponentSchemaSet schemas;
        simulation::SimulationSystemRegistry systems;
        auto builder = runtime.builder();
        builder.setDescription(std::make_shared<const scene::SceneDescription>(std::move(*resolved)))
            .setWorld(std::make_shared<const world::WorldDescription>())
            .setSimulation(std::make_shared<const simulation::SimulationDescription>())
            .setRegistrations(schemas, systems, {});
        auto instance = builder.build();
        assert(instance);
        CloseProbe probe(engine.execution());
        auto closing =
            object::LuxObject::connect(&engine, &engine::Engine::sceneClosing, &probe, &CloseProbe::onClosing);
        auto closed = object::LuxObject::connect(&engine, &engine::Engine::sceneClosed, &probe, &CloseProbe::onClosed);
        assert(closing && closed);
        assert(runtime.addCloseParticipant(
            *instance,
            {{},
             "first",
             {},
             &probe,
             +[](void* p, scene::SceneInstanceId) noexcept { return static_cast<CloseProbe*>(p)->finishFirst(); }}
        ));
        assert(runtime.addCloseParticipant(
            *instance,
            {{},
             "second",
             {"first"},
             &probe,
             +[](void* p, scene::SceneInstanceId) noexcept { return static_cast<CloseProbe*>(p)->finishSecond(); }}
        ));
        assert(runtime.addCloseParticipant(
            *instance,
            {{},
             "independent",
             {},
             &probe,
             +[](void* p, scene::SceneInstanceId) noexcept { return static_cast<CloseProbe*>(p)->finishIndependent(); }}
        ));
        auto retired = runtime.retireInstance(*instance);
        assert(retired && runtime.retireInstance(*instance));
        assert(!runtime.addCloseParticipant(
            *instance,
            {{},
             "late",
             {},
             &probe,
             +[](void* p, scene::SceneInstanceId) noexcept { return static_cast<CloseProbe*>(p)->finishFirst(); }}
        ));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        auto drive = [&]
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(engine.execution().dispatchMainCompletions());
            assert(runtime.driveFrame(scene::ESceneDriveMode::ADVANCE));
        };
        while (retired->failure().type == 0)
        {
            drive();
        }
        assert(
            !retired->complete() && probe.first == 1 && probe.second == 1 && probe.closing == 1 && probe.closed == 0
        );
        assert(runtime.borrowClock(*instance)); // Failure preserves the actual instance.
        auto repeated = runtime.retireInstance(*instance);
        assert(repeated && repeated->failure() == retired->failure());
        drive();
        assert(probe.second == 1);
        probe.fail = false;
        assert(runtime.retryClose(*instance));
        while (!retired->complete())
        {
            drive();
        }
        assert(probe.first == 1 && probe.second == 2 && probe.closing == 1 && probe.closed == 1);
        // A fully closed Runtime rejects construction without publishing a candidate.
        assert(runtime.requestClose());
        assert(!builder.build());
        assert(probe.independent == 1 && probe.closing == 1 && probe.closed == 1);
        std::puts("PASS Scene close: declared dependency, delayed completion, failure retention, exact retry, one "
                  "closing/closed and closed-runtime rejection");
    }

    struct RetirementState final
    {
        execution::TaskScope& tasks;
        bool& destroyed;
        bool completed = false;

        ~RetirementState()
        {
            destroyed = true;
        }
    };

    struct ProjectScene final
    {

        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.editor.retirement",
            .version = 1
        };
        inline static RetirementState* input;
        RetirementState& state = *input;

        ~ProjectScene()
        {
            assert(state.tasks.submit(
                {},
                [state = &state](execution::TaskReporter) noexcept
                {
                    auto ready = stdexec::then(
                        stdexec::schedule(state->tasks.execution().mainScheduler()),
                        [state]() noexcept
                        {
                            assert(!state->destroyed);
                            state->completed = true;
                        }
                    );
                    return stdexec::write_env(
                        std::move(ready),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    );
                }
            ));
        }
    };
} // namespace

int main()
{
    auto& owner = object::ObjectRuntime::instance();
    auto engine = engine::Engine::create({1, 64, {32}, execution::BlockingSchedulerConfig{1, 32}}, 64);
    cxx::scope_exit close_engine([&]() noexcept { assert((*engine)->close()); });
    assert(engine && owner.isCurrent());
    checkSceneClose(**engine);
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("20d09a0e-ac42-4280-be98-6e31f51c0cfe")};
    const ProjectDescription description{"Complete", std::filesystem::current_path()};
    const ProjectManifest manifest{1, ids("complete"), description.name};
    detail::PreparedProject incomplete;
    incomplete.description = description;
    incomplete.description.manifest_file = description.root / "Complete.luxproj";
    incomplete.manifest = manifest;
    EditorComposition declarations;
    auto invalid = detail::createEditorContext(**engine, std::move(incomplete), std::move(declarations));
    assert(!invalid && invalid.error().type == Errors::ProjectNeedsPreparedPlugins);
    unsigned constructed{};
    auto rejected = fixture::createContext(
        **engine,
        description,
        manifest,
        [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            assert(composition.registerServiceFactory<int>(
                [&](EditorContext&) noexcept -> FrameworkResult<std::unique_ptr<int>>
                {
                    ++constructed;
                    return std::make_unique<int>(1);
                }
            ));
            return cxx::unexpected(error::Error{Errors::EditorInvalidUiFactory});
        }
    );
    assert(!rejected && constructed == 0);
    auto context = fixture::createContext(
        **engine,
        description,
        manifest,
        [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; }
    );
    assert(context);
    static_assert(std::is_same_v<decltype((*context)->plugins()), const plugins::PluginManager&>);
    static_assert(std::is_same_v<decltype((*context)->sceneRegistrations()), const plugins::SceneRegistrations&>);
    assert((*context)->sceneRegistrations().scene_systems.empty());
    assert((*context)->project().manifest_file.is_absolute());

    struct Work final
    {
        std::binary_semaphore started{0};
        std::binary_semaphore release{0};
        bool delivered{};
    };

    auto work = std::make_shared<Work>();
    auto scheduler = (*engine)->execution().blockingIoScheduler();
    assert(scheduler);
    assert((*context)->tasks().submit(
        {"Context destruction", "test"},
        [&,
         make_work =
             [work, scheduler = *scheduler](execution::TaskReporter reporter) noexcept
         {
             return stdexec::then(
                 stdexec::schedule(scheduler),
                 [work, reporter]() noexcept -> FrameworkResult<void>
                 {
                     work->started.release();
                     work->release.acquire();
                     assert(reporter.stopToken().stop_requested());
                     return {};
                 }
             );
         },
         consume_result =
             [work, thread = std::this_thread::get_id()](
                 execution::ExecutionResult<void, error::Error>&& result
             ) noexcept
         {
             assert(result && thread == std::this_thread::get_id() && !work->delivered);
             work->delivered = true;
         }](lux::execution::TaskReporter reporter) mutable noexcept
        {
            auto ready = stdexec::continues_on(
                lux::execution::asResult(make_work(reporter)),
                (*engine)->execution().mainScheduler()
            );
            return stdexec::then(
                std::move(ready),
                [consume_result = std::move(consume_result)](auto result) mutable noexcept
                {
                    const auto outcome = lux::execution::taskState(result);
                    consume_result(std::move(result));
                    return outcome;
                }
            );
        }
    ));
    assert(work->started.try_acquire_for(5s));
    std::binary_semaphore destroyed{0};
    std::atomic_bool waited{};
    std::thread watchdog(
        [&]
        {
            if (!destroyed.try_acquire_for(2s))
            {
                waited = true;
                work->release.release();
            }
        }
    );
    context->reset();
    destroyed.release();
    watchdog.join();
    assert(!waited && !work->delivered);
    work->release.release();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!work->delivered)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        static_cast<void>((*engine)->execution().dispatchMainCompletions());
        std::this_thread::yield();
    }
    bool provider_destroyed = false;
    auto scene_context = fixture::createContext(
        **engine,
        description,
        manifest,
        [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            return composition.registerServiceFactory<RetirementState>(
                [&](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<RetirementState>>
                { return std::unique_ptr<RetirementState>{new RetirementState{context.tasks(), provider_destroyed}}; }
            );
        }
    );
    assert(scene_context);
    auto provider = (*scene_context)->service<RetirementState>();
    assert(provider);
    ProjectScene::input = &provider->get();
    const scene::SceneSystemRegistration registration{
        .type = system::systemTypeId(ProjectScene::Description.canonical_name),
        .cpp_type = cxx::make_basic_type_info<ProjectScene>(),
        .description = &ProjectScene::Description,
        .install = [](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription description
                   ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure>
        {
            auto system = installer.emplaceSystem<ProjectScene>(description.instanceId());
            return system ? cxx::expected<void, scene::SceneSystemBuildFailure>{} : cxx::unexpected(system.error());
        }
    };
    scene::SceneDescriptionBuilder authored;
    assert(authored.addSystem({1}, "retirement", registration.type, 1, {}, 0));
    auto resolved = std::move(authored).buildResolved();
    assert(resolved);
    const ecs::ComponentSchemaSet components;
    const simulation::SimulationSystemRegistry systems;
    auto builder = (*engine)->sceneRuntime().builder();
    builder.setDescription(std::make_shared<const scene::SceneDescription>(std::move(*resolved)))
        .setWorld(std::make_shared<const world::WorldDescription>())
        .setSimulation(std::make_shared<const simulation::SimulationDescription>())
        .setRegistrations(components, systems, std::span{&registration, 1});
    auto instance = (*scene_context)->createScene(builder);
    assert(instance);
    (*scene_context)->close();
    assert(!(*scene_context)->finishClose() && !provider_destroyed);
    auto driven = (*engine)->sceneRuntime().driveFrame();
    assert(driven && driven->empty());
    assert(!(*scene_context)->finishClose() && !provider_destroyed);
    assert((*engine)->execution().dispatchMainCompletions());
    assert(provider->get().completed && (*scene_context)->finishClose());
    scene_context->reset();
    assert(provider_destroyed);
    std::puts("PASS complete Context, assembly refusal before construction, blocked task outlives Context without wait"
    );
}
