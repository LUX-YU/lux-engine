#include "api_contract.hpp"
#include "support/PreparedProjectFixture.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
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
    auto engine = engine::EngineContext::create({1, 64, {32}, execution::BlockingSchedulerConfig{1, 32}}, 64);
    assert(engine && owner.isCurrent());
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
    static_assert(std::is_same_v<decltype((*context)->plugins()), const project::PluginManager&>);
    static_assert(std::is_same_v<decltype((*context)->sceneRegistrations()), const project::SceneRegistrations&>);
    assert((*context)->sceneRegistrations().scene_systems.empty());
    assert((*context)->project().manifest_file.is_absolute());

    struct Work final
    {
        std::binary_semaphore started{0};
        std::binary_semaphore release{0};
        bool delivered{};
    };

    auto work = std::make_shared<Work>();
    auto scheduler = (*engine)->execution().blocking();
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
             [work, thread = std::this_thread::get_id()](execution::TTaskResult<void, error::Error>&& result) noexcept
         {
             assert(result && thread == std::this_thread::get_id() && !work->delivered);
             work->delivered = true;
         }](lux::execution::TaskReporter reporter) mutable noexcept
        {
            auto ready = stdexec::continues_on(
                lux::execution::taskResult(make_work(reporter)),
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
        .cpp_type = cxx::typeToken<ProjectScene>(),
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
    const simulation::ecs::ComponentSchemaSet components;
    const simulation::SimulationSystemRegistry systems;
    auto instance = (*engine)
                        ->sceneRuntime()
                        .builder()
                        .setDescription(std::make_shared<const scene::SceneDescription>(std::move(*resolved)))
                        .setWorld(std::make_shared<const world::WorldDescription>())
                        .setSimulation(std::make_shared<const simulation::SimulationDescription>())
                        .setRegistrations(components, systems, std::span{&registration, 1})
                        .build();
    assert(instance && (*scene_context)->adoptScene(std::move(*instance)));
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
