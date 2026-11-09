#include "../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "support/ProjectRequests.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/Engine.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/EditorApplication.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/systems/render/RenderExecution.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>
#include <semaphore>
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
        inline static std::uint64_t created{};

        explicit Service(std::vector<int>& counts) : deaths(counts)
        {
            ++created;
        }

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
            ++created;
            assert(content_.addElement(label_) && content_.addElement(draw_probe_) && addElement(content_));
        }

        inline static std::uint64_t draws{}, created{};

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
        assert(!fixture::createLoadedEditor({"Invalid extent", 0, 480}));
        EditorConfig invalid;
        invalid.layout = defaultProjectLayout({{"test", "same", "One"}, {"test", "same", "Two"}});
        assert(!fixture::createLoadedEditor(invalid));
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
                            [&,
                             make_work = [](execution::TaskReporter) noexcept
                             { return stdexec::just(FrameworkResult<void>{}); },
                             consume_result =
                                 [&](execution::ExecutionResult<void, error::Error>&& result) noexcept
                             {
                                 // Cleanup can start after this completion. C's Context/service must remain alive
                                 // either way.
                                 assert(result && std::ranges::count(deaths, 2) == 1);
                                 rejected_candidate_delivered = true;
                             }](lux::execution::TaskReporter reporter) mutable noexcept
                            {
                                auto ready = stdexec::continues_on(
                                    lux::execution::asResult(make_work(reporter)),
                                    context.tasks().execution().mainScheduler()
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
                        return cxx::unexpected(error::Error{FixtureErrors::EditorExpectedSecondFactoryRefusal});
                    }
                    return std::unique_ptr<ui::Pane>{new TestPane(description.name, deaths)};
                }
            );
        };
        EditorConfig config{"Framework project qualification", 640, 480};
        config.enable_validation = true;
        config.layout = defaultProjectLayout({{"test", "one", "One"}, {"test", "two", "Two"}});
        config.plugin_locations = {{catalog, plugin_root}};
        config.project_file = directory / "A/Project.luxproj";
        ProjectStartup startup{manifest("A"), {}};
        auto engine = take(fixture::createLoadedEditor(std::move(config), std::move(assembly), std::move(startup)));
        auto* runtime = &engine->engine();
        auto* window = &engine->window();
        auto* root = &window->uiRoot();

        class Listener final : public object::LuxObject
        {
        public:
            explicit Listener(EditorApplication& host) : host_(host) {}

            void attached(const ui::PaneChanged&) noexcept
            {
                // Structural facts observe the semantic owner published by on_commit.
                assert(host_.engine().closing() || !host_.project().project().name.empty());
                ++calls;
            }

            unsigned calls{};

        private:
            EditorApplication& host_;
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
                assert(take(fixture::stepEditor(*engine)) == true);
                std::this_thread::sleep_for(1ms);
            }
        };
        const auto settled = [&] { return fixture::preparationsFinished(*engine); };
        assert(engine->project().project().name == "A");
        assert(facts.changed == 0 && facts.failed == 0);
        assert(readProjectManifest(directory / "A/Project.luxproj"));
        until([&] { return TestPane::draws >= 4 && engine->rendering().stats().presentations > 0; });
        auto* original = &engine->project();
        assert(!fixture::open(*engine, {}) && &engine->project() == original);
        assert(fixture::open(*engine, directory / "missing.luxproj"));
        until(settled);
        assert(facts.failed != 0 && &engine->project() == original);
        auto broken = manifest("MissingPlugin");
        broken.plugins = {{"test.no_such_plugin", 1}};
        assert(fixture::open(*engine, write(broken)));
        until(settled);
        assert(facts.failed != 0 && &engine->project() == original);
        assert(facts.failure.error.type == Errors::ProjectPlugins);
        const auto d = write(manifest("D"));
        assert(fixture::open(*engine, d));
        auto busy = fixture::open(*engine, d);
        assert(!busy && busy.error().type == Errors::ProjectLoadBusy);
        fixture::cancel(*engine);
        until(settled);
        assert(facts.failed == 2 && &engine->project() == original);
        assert(deaths.empty() && ui_test::paneCount(*root) == 3);
        // Finish worker preparation without collecting it, then cancel. The manifest publication
        // remains a disk fact even though the prepared Context must never be adopted.
        execution::TaskScope preparation_gate(runtime->execution());
        std::atomic_uint waiting_workers{};
        std::counting_semaphore<2> release_workers{0};
        auto blocking = runtime->execution().blockingIoScheduler();
        assert(blocking);
        for (unsigned i = 0; i != 2; ++i)
        {
            assert(preparation_gate.submit(
                {"Hold preparation worker", "test"},
                [&](execution::TaskReporter) noexcept
                {
                    return stdexec::schedule(*blocking) | stdexec::then(
                                                              [&]() noexcept
                                                              {
                                                                  ++waiting_workers;
                                                                  release_workers.acquire();
                                                              }
                                                          );
                }
            ));
        }
        until([&] { return waiting_workers == 2; });
        assert(fixture::create(*engine, {directory / "PublishedCancelled", manifest("PublishedCancelled")}));
        // Enter the shared Main stage once; the blocking worker can then finish without adopting its result.
        assert(fixture::stepEditor(*engine));
        assert(fixture::stepEditor(*engine));
        release_workers.release(2);
        const auto prepared_deadline = std::chrono::steady_clock::now() + 20s;
        for (;;)
        {
            if (std::filesystem::exists(directory / "PublishedCancelled/Project.luxproj") &&
                runtime->execution().hasReadyWork())
            {
                break;
            }
            assert(std::chrono::steady_clock::now() < prepared_deadline);
            std::this_thread::sleep_for(1ms);
        }
        fixture::cancel(*engine);
        until(settled);
        assert(facts.failed == 2);
        assert(&engine->project() == original);
        assert(readProjectManifest(directory / "PublishedCancelled/Project.luxproj"));
        assert(deaths.empty());
        until([&] { return preparation_gate.settled(); });

        // A real accepted task is held after stop. New UI is published, but A's service owner remains
        // until its accepted completion drains; the application still pumps frames while waiting.
        std::atomic_bool started{}, release{};
        bool delivered{};
        auto scheduler = runtime->execution().blockingIoScheduler();
        assert(scheduler);
        assert(original->tasks().submit(
            {"Project close negative", "test"},
            [&,
             make_work =
                 [&](execution::TaskReporter reporter) noexcept
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
             consume_result =
                 [&](execution::ExecutionResult<void, error::Error>&& result) noexcept
             {
                 assert(result && (deaths == std::vector<int>{1, 1}));
                 delivered = true;
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready = stdexec::continues_on(
                    lux::execution::asResult(make_work(reporter)),
                    runtime->execution().mainScheduler()
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
        until([&] { return started.load(); });
        auto b = manifest("B");
        b.plugins = {{"lux.builtin.scene_render", 2}};
        assert(fixture::open(*engine, write(b)));
        until([&] { return engine->project().project().name == "B"; });
        assert(!delivered && (deaths == std::vector<int>{1, 1}));
        assert(engine->state() == EApplicationState::LOADING_PROJECT);
        for (int i = 0; i < 3; ++i)
        {
            assert(take(fixture::stepEditor(*engine)) == true);
            assert(engine->project().project().name == "B" && !delivered);
        }
        release = true;
        until(settled);
        assert(delivered && engine->project().project().name == "B");
        assert((deaths == std::vector<int>{1, 1, 2}));
        assert(!engine->project().sceneRegistrations().scene_systems.empty());
        assert(&engine->engine() == runtime && &engine->window() == window && &window->uiRoot() == root);
        assert(
            runtime->sceneRuntime().instanceCount() == 0
        ); // UI is an independent Render Scene, never an empty Engine World.
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
            assert(take(fixture::stepEditor(*engine)) == true);
        }
        assert(TestPane::draws == before);
        ShowWindow(static_cast<HWND>(window->nativeHandle()), SW_RESTORE);
        until([&] { return !window->minimized() && TestPane::draws > before; });
#endif
        auto* b_context = &engine->project();
        assert(fixture::open(*engine, write(manifest("C"))));
        until(settled);
        assert(facts.failed == 3);
        assert(facts.failure.error.type == FixtureErrors::EditorExpectedSecondFactoryRefusal);
        assert(&engine->project() == b_context && ui_test::paneCount(*root) == 3);
        until([&] { return rejected_candidate_delivered; });
        assert((deaths == std::vector<int>{1, 1, 2, 1, 2})); // Only C's detached candidates were destroyed.
        assert(fixture::open(*engine, d));
        until(settled);
        assert(engine->project().project().name == "D");
        assert((deaths == std::vector<int>{1, 1, 2, 1, 2, 1, 1, 2}));
        until([&] { return TestPane::draws > before + 2; });
        const auto statistics = engine->rendering().stats();
        assert(statistics.presentations > 0);
        assert(fixture::open(*engine, directory / "A/Project.luxproj"));
        engine->engine().requestClose(); // Late preparation is drained/cancelled; A is never adopted after close.
        const auto exit_deadline = std::chrono::steady_clock::now() + 20s;
        while (take(fixture::stepEditor(*engine)) != false)
        {
            assert(std::chrono::steady_clock::now() < exit_deadline);
            std::this_thread::sleep_for(1ms);
        }
        assert(ui_test::paneCount(*root) == 0);
        assert(listener.calls > 0);
        connection->disconnect();
        engine.reset();
        assert(std::ranges::count(deaths, 1) == TestPane::created);
        assert(std::ranges::count(deaths, 2) == Service::created);
        assert(deaths.size() == TestPane::created + Service::created && deaths.back() == 2);
        std::filesystem::remove_all(directory);
        std::printf(
            "PASS real project create/prepare/cancel/plugins/async close, failed C preserves B, GPU %llu frames\n",
            static_cast<unsigned long long>(statistics.presentations)
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
