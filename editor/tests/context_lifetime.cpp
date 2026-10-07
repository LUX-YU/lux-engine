#include "api_contract.hpp"
#include "support/PreparedProjectFixture.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;

int main()
{
    auto& owner = object::ObjectRuntime::instance();
    auto engine = engine::EngineContext::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 32}}, {0, 64});
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
        [work, scheduler = *scheduler](process::TaskReporter reporter) noexcept
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
        [work, thread = std::this_thread::get_id()](process::TTaskResult<void, error::Error>&& result) noexcept
        {
            assert(result && thread == std::this_thread::get_id() && !work->delivered);
            work->delivered = true;
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
        static_cast<void>((*engine)->execution().collectCompletions());
        std::this_thread::yield();
    }
    std::puts("PASS complete Context, assembly refusal before construction, blocked task outlives Context without wait"
    );
}
