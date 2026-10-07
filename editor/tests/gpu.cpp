#include "../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "support/ProjectRequests.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;
namespace FixtureErrors
{
    inline constexpr lux::error::ErrorId EditorExpectedSecondFactoryRefusal =
        lux::error::errorId("lux.editor.expected_second_factory_refusal");
}

namespace
{
    struct Service final
    {
        std::vector<int>& deaths;
        ~Service()
        {
            deaths.push_back(2);
        }
    };
    class TestPane final : public ui::Pane
    {
    public:
        TestPane(std::string name, std::vector<int>& deaths)
            : Pane(std::move(name)), deaths_(deaths), content_{}, label_("Actual UI GPU content"), draw_probe_(*this)
        {
            assert(content_.addElement(label_) && content_.addElement(draw_probe_) && addElement(content_));
        }
        inline static std::uint64_t draws{};
        void drawTestContent(ui::Element&) noexcept
        {
            ++draws;
        }
        ~TestPane() override
        {
            deaths_.push_back(1);
        }

    private:
        std::vector<int>& deaths_;
        ui::Layout content_;
        ui::Label label_;
        TUiTestContent<TestPane> draw_probe_;
    };
    template <class T> T take(FrameworkResult<T> result)
    {
        if (!result)
        {
            std::fprintf(stderr, "framework error: %s\n", error::format(result.error()).c_str());
        }
        assert(result);
        return std::move(*result);
    }
    void nativeLifecycle(const std::filesystem::path& catalog, const std::filesystem::path& plugin_root)
    {
        assert(!LuxEngine::create({"Invalid extent", 0, 480}));
        EditorConfig invalid;
        invalid.layout = {{"test", "same", "One"}, {"test", "same", "Two"}};
        assert(!LuxEngine::create(invalid));
        std::mt19937 random{std::random_device{}()};
        uuids::uuid_random_generator generate(random);
        const auto directory = std::filesystem::temp_directory_path() / ("lux-project-" + uuids::to_string(generate()));
        std::filesystem::create_directories(directory);
        const auto manifest = [&](std::string name) { return ProjectManifest{1, generate(), std::move(name)}; };
        const auto write = [&](const ProjectManifest& value)
        {
            const auto file = directory / (value.name + ".luxproj");
            assert(writeProjectManifestAtomic(file, value, EProjectWrite::CREATE));
            return file;
        };
        std::vector<int> deaths;
        bool rejected_candidate_delivered{};
        auto assembly = [&, owned = std::make_unique<int>(42)](EditorComposition& context
                        ) noexcept -> FrameworkResult<void>
        {
            assert(*owned == 42); // Real move-only assembly survives every asynchronous open.
            assert(context.registerServiceFactory<Service>(
                [&](EditorContext&) noexcept -> FrameworkResult<std::unique_ptr<Service>>
                { return std::make_unique<Service>(deaths); }
            ));
            return context.registerUiFactory(
                "test",
                [&](EditorContext& context,
                    const PaneDescription& description) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    assert(context.service<Service>());
                    if (context.project().name == "C" && description.name == "two")
                    {
                        assert(context.tasks().submit(
                            {"Rejected candidate completion", "test"},
                            [](process::TaskReporter) noexcept { return stdexec::just(FrameworkResult<void>{}); },
                            [&](process::TTaskResult<void, error::Error>&& result) noexcept
                            {
                                // C's UI and Context are gone. Completion owns its result, not the failed candidate.
                                assert(result && (deaths == std::vector<int>{1, 1, 2, 1, 2}));
                                rejected_candidate_delivered = true;
                            }
                        ));
                        return cxx::unexpected(error::Error{FixtureErrors::EditorExpectedSecondFactoryRefusal});
                    }
                    return std::unique_ptr<ui::Pane>{new TestPane(description.name, deaths)};
                }
            );
        };
        EditorConfig config{"Framework project qualification", 640, 480};
        config.layout = {{"test", "one", "One"}, {"test", "two", "Two"}};
        config.plugin_locations = {{catalog, plugin_root}};
        auto engine = take(LuxEngine::create(std::move(config), std::move(assembly)));
        auto* runtime = &engine->engine();
        auto* window = &engine->window();
        auto* root = &window->uiRoot();
        class Listener final : public object::LuxObject
        {
        public:
            explicit Listener(LuxEngine& host) : host_(host) {}
            void attached(const ui::PaneChanged&) noexcept
            {
                // Structural facts observe the semantic owner published by on_commit.
                assert(host_.project() || host_.window().shouldClose());
                ++calls;
            }
            unsigned calls{};

        private:
            LuxEngine& host_;
        } listener(*engine);
        auto connection = object::LuxObject::connect(root, &ui::Root::paneChanged, &listener, &Listener::attached);
        assert(connection);
        fixture::ProjectFacts facts{*engine};
        unsigned waiting{};
        auto until = [&](auto predicate)
        {
            std::fprintf(stderr, "GPU wait %u\n", ++waiting);
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (!predicate())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                assert(take(engine->frame()) == EFrameStatus::RUNNING);
                std::this_thread::sleep_for(1ms);
            }
        };
        const auto settled = [&] { return fixture::preparationsFinished(*engine); };
        assert(fixture::create(*engine, {directory / "A", manifest("A")}));
        assert(!engine->project());
        until(settled);
        assert(facts.changed == 1 && facts.failed == 0);
        assert(readProjectManifest(directory / "A/Project.luxproj"));
        until([&] { return TestPane::draws >= 4 && runtime->renderContext()->runtime().statistics().frames > 0; });
        auto* original = engine->project();
        assert(!fixture::open(*engine, {}) && engine->project() == original);
        assert(fixture::open(*engine, directory / "missing.luxproj"));
        until(settled);
        assert(facts.failed != 0 && engine->project() == original);
        auto broken = manifest("MissingPlugin");
        broken.plugins = {{"test.no_such_plugin", 1}};
        assert(fixture::open(*engine, write(broken)));
        until(settled);
        assert(facts.failed != 0 && engine->project() == original);
        assert(facts.failure.error.type == Errors::ProjectPlugins);
        const auto d = write(manifest("D"));
        assert(fixture::open(*engine, d));
        assert(fixture::open(*engine, d)); // Latest intent replaces the previous one.
        fixture::cancel(*engine);
        until(settled);
        assert(facts.failed == 2 && engine->project() == original);
        assert(deaths.empty() && ui_test::paneCount(*root) == 2);
        // Finish worker preparation without collecting it, then cancel. The manifest publication
        // remains a disk fact even though the prepared Context must never be adopted.
        assert(fixture::create(*engine, {directory / "PublishedCancelled", manifest("PublishedCancelled")}));
        const auto prepared_deadline = std::chrono::steady_clock::now() + 20s;
        for (;;)
        {
            if (std::filesystem::exists(directory / "PublishedCancelled/Project.luxproj") &&
                object::ObjectRuntime::instance().statistics().pending != 0)
            {
                break;
            }
            assert(std::chrono::steady_clock::now() < prepared_deadline);
            std::this_thread::sleep_for(1ms);
        }
        fixture::cancel(*engine);
        until(settled);
        assert(facts.failed == 2);
        assert(engine->project() == original);
        assert(readProjectManifest(directory / "PublishedCancelled/Project.luxproj"));
        assert(deaths.empty());

        // A real accepted task is deliberately held after stop. Switching must keep A's services alive
        // and return from frame(); the test releases the worker only after observing several frames.
        std::atomic_bool started{}, release{};
        bool delivered{};
        auto scheduler = runtime->execution().blocking();
        assert(scheduler);
        assert(original->tasks().submit(
            {"Project close negative", "test"},
            [&](process::TaskReporter reporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(*scheduler),
                    [&, reporter]() noexcept -> FrameworkResult<void>
                    {
                        started = true;
                        while (!release.load())
                        {
                            std::this_thread::yield();
                        }
                        assert(reporter.stopToken().stop_requested());
                        return {};
                    }
                );
            },
            [&](process::TTaskResult<void, error::Error>&& result) noexcept
            {
                assert(result && (deaths == std::vector<int>{1, 1, 2}));
                delivered = true;
            }
        ));
        until([&] { return started.load(); });
        auto b = manifest("B");
        b.plugins = {{"lux.builtin.scene_render", 1}};
        assert(fixture::open(*engine, write(b)));
        until([&] { return engine->project() && engine->project()->project().name == "B"; });
        assert(!delivered && (deaths == std::vector<int>{1, 1, 2}));
        for (int i = 0; i < 3; ++i)
        {
            assert(take(engine->frame()) == EFrameStatus::RUNNING);
            assert(engine->project()->project().name == "B" && !delivered);
        }
        release = true;
        until([&] { return delivered; });
        assert(delivered && engine->project()->project().name == "B");
        assert((deaths == std::vector<int>{1, 1, 2}));
        assert(!engine->project()->sceneRegistrations().features.empty());
        assert(&engine->engine() == runtime && &engine->window() == window && &window->uiRoot() == root);
        assert(runtime->sceneRuntime().instanceCount() == 1); // Only the original UI Scene; no project Scene yet.
        auto state = window->state();
        assert(state);
        auto placement = state->placement;
        placement.normal.width = 720;
        placement.normal.height = 520;
        assert(window->applyPlacement(placement));
        auto before = TestPane::draws;
        until([&] { return TestPane::draws > before + 2; });
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_MINIMIZE);
        until([&] { return window->minimized(); });
        before = TestPane::draws;
        for (int i{}; i < 5; ++i)
        {
            assert(take(engine->frame()) == EFrameStatus::RUNNING);
        }
        assert(TestPane::draws == before);
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_RESTORE);
        until([&] { return !window->minimized() && TestPane::draws > before; });
#endif
        auto* b_context = engine->project();
        assert(fixture::open(*engine, write(manifest("C"))));
        until(settled);
        assert(facts.failed == 3);
        assert(facts.failure.error.type == FixtureErrors::EditorExpectedSecondFactoryRefusal);
        assert(engine->project() == b_context && ui_test::paneCount(*root) == 2);
        until([&] { return rejected_candidate_delivered; });
        assert((deaths == std::vector<int>{1, 1, 2, 1, 2})); // Only C's detached candidates were destroyed.
        assert(fixture::open(*engine, d));
        until(settled);
        assert(engine->project()->project().name == "D");
        assert((deaths == std::vector<int>{1, 1, 2, 1, 2, 1, 1, 2}));
        until([&] { return TestPane::draws > before + 2; });
        const auto statistics = runtime->renderContext()->runtime().statistics();
        assert(statistics.frames > 0);
        assert(fixture::open(*engine, directory / "A/Project.luxproj"));
        window->exit(); // Late preparation is drained/cancelled; A is never adopted after native close.
        const auto exit_deadline = std::chrono::steady_clock::now() + 20s;
        while (take(engine->frame()) != EFrameStatus::EXIT_REQUESTED)
        {
            assert(std::chrono::steady_clock::now() < exit_deadline);
            std::this_thread::sleep_for(1ms);
        }
        assert(!engine->project());
        assert(listener.calls > 0);
        connection->disconnect();
        engine.reset();
        assert(deaths.size() == 11 && deaths.back() == 2);
        std::filesystem::remove_all(directory);
        std::printf(
            "PASS real project create/prepare/cancel/plugins/async close, failed C preserves B, GPU %llu frames\n",
            static_cast<unsigned long long>(statistics.frames)
        );
    }

} // namespace
int main(int argc, char** argv)
{
    const lux::error::ErrorDescriptor fixture_errors[]{
        {"lux.editor.expected_second_factory_refusal",
         "Expected second factory refusal",
         lux::error::ERecovery::PERMANENT}
    };
    assert(lux::error::ErrorRegistry::instance().registerTypes(fixture_errors));

    assert(argc == 4);
    nativeLifecycle(argv[2], argv[3]);
}
