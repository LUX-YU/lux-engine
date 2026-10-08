#include "../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "../api_contract.hpp"
#include "../support/ProjectRequests.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <map>
#include <semaphore>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;
namespace
{
    struct Counts final
    {
        unsigned updates{}, draws{}, panes{}, services{};
    };
    struct Service final
    {
        Counts& counts;
        std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
        ~Service()
        {
            *alive = false;
            ++counts.services;
        }
    };
    struct Pane final : ui::Pane
    {
        Pane(EditorContext& context, Counts& counts)
            : ui::Pane("Project"), context(context), counts(counts), draw(*this)
        {
            auto service = context.service<Service>();
            assert(service);
            alive = service->get().alive;
            assert(addElement(draw));
            if (context.project().name == "C")
            {
                beginDestruction();
            }
        }
        ~Pane() override
        {
            assert(*alive && !context.project().name.empty());
            ++counts.panes;
        }
        void update() noexcept override
        {
            ++counts.updates;
        }
        void drawTestContent(ui::Element&) noexcept
        {
            ++counts.draws;
        }
        EditorContext& context;
        Counts& counts;
        TUiTestContent<Pane> draw;
        std::shared_ptr<const bool> alive;
    };
    struct Global final : ui::Pane
    {
        explicit Global(cxx::move_only_function<void() noexcept> step) : ui::Pane("Global"), step(std::move(step)) {}
        void update() noexcept override
        {
            ++updates;
            step();
        }
        cxx::move_only_function<void() noexcept> step;
        unsigned updates{};
    };
    template <class T>
    concept PublicFrame = requires(T& value) { value.frame(); };
    template <class T>
    concept PublicExec = requires(T& value) { value.exec(); };
    template <class T>
    concept PublicPump = requires(T& value) { value.pumpOnce(); };
    template <class T>
    concept PublicStatistics = requires(T& value) { value.statistics(); };
    template <class T>
    concept PublicResizeSlot = requires(T& value) { value.on_resize; };
    static_assert(!PublicFrame<LuxEngine> && !PublicExec<LuxEngine> && !PublicPump<LuxEngine>);
    static_assert(!PublicStatistics<LuxEngine> && !PublicResizeSlot<EditorWindow>);
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("7f5573fa-e4e6-46a7-9376-aa4580135d11")};
    auto file = [&](std::string name)
    {
        auto path = directory / (name + ".luxproj");
        assert(writeProjectManifestAtomic(path, {1, ids(name), name}, EProjectWrite::REPLACE));
        return path;
    };
    const auto a = file("A"), b = file("B"), c = file("C");
    std::map<std::string, Counts> counts;
    EditorConfig config{"Public SDK host", 640, 480};
    config.layout = {{"probe", "project", "Project"}};
    auto made = LuxEngine::create(
        std::move(config),
        [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            assert(composition.registerServiceFactory<Service>(
                [&](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<Service>>
                { return std::make_unique<Service>(counts[context.project().name]); }
            ));
            return composition.registerUiFactory(
                "probe",
                [&](EditorContext& context,
                    const PaneDescription&) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    auto candidate = std::make_unique<Pane>(context, counts[context.project().name]);
                    return candidate;
                }
            );
        }
    );
    assert(made);
    auto host = std::move(*made);
    auto* engine_identity = &host->engine();
    auto* window_identity = &host->window();
    auto& root = window_identity->uiRoot();
    fixture::ProjectFacts facts(*host);
    std::binary_semaphore release{0};
    std::atomic_bool started{};
    bool delivered{};
    unsigned phase{}, old_updates{}, global_at_b{};
    unsigned reported_phase = 100;
    std::uint64_t revision{}, draws_before{};
    ui::PaneHandle global_handle;
    Global* global{};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 20s;
    process::TaskScope observation_tasks(engine_identity->execution());
    auto after = [&](auto delay, auto callback) noexcept
    {
        auto& execution = engine_identity->execution();
        auto accepted = observation_tasks.submit(
            {"Public host observation", "test"},
            [&, delay](process::TaskReporter) noexcept
            {
                using Result = cxx::expected<void, process::ETimerError>;
                return stdexec::upon_error(
                    stdexec::then(execution.timer().after(delay), []() noexcept { return Result{}; }),
                    [](process::ETimerError error) noexcept -> Result { return cxx::unexpected(error); }
                );
            },
            [callback = std::move(callback)](process::TTaskResult<void, process::ETimerError>&& result) noexcept
            {
                assert(result);
                callback();
            }
        );
        assert(accepted);
    };
    auto step = [&]() noexcept
    {
        if (reported_phase != phase)
        {
            std::fprintf(
                stderr,
                "Public host phase %u: metrics %u x %u minimized=%d\n",
                phase,
                window_identity->metrics().width,
                window_identity->metrics().height,
                window_identity->metrics().minimized
            );
            reported_phase = phase;
        }
        assert(std::chrono::steady_clock::now() < deadline);
        assert(&host->engine() == engine_identity && &host->window() == window_identity);
        assert(root.resolvePane(global_handle) == global);
        auto* project = host->project();
        switch (phase)
        {
        case 0:
            if (!project || counts["A"].draws < 3)
            {
                return;
            }
            assert(project->project().manifest_file == std::filesystem::canonical(a));
            assert(engine_identity->sceneRuntime().instanceCount() == 1);
            {
                const auto scheduler = project->tasks().execution().blocking();
                assert(scheduler);
                assert(project->tasks().submit(
                    {"Held project task", "test"},
                    [&, scheduler = *scheduler](process::TaskReporter reporter) noexcept
                    {
                        return stdexec::schedule(scheduler) | stdexec::then(
                                                                  [&, reporter]() noexcept -> FrameworkResult<void>
                                                                  {
                                                                      started = true;
                                                                      release.acquire();
                                                                      assert(reporter.stopToken().stop_requested());
                                                                      return {};
                                                                  }
                                                              );
                    },
                    [&](process::TTaskResult<void, error::Error>&& result) noexcept
                    {
                        assert(result && counts["A"].services == 1 && counts["A"].panes == 1);
                        delivered = true;
                    }
                ));
            }
            phase = 1;
            return;
        case 1:
            if (!started)
            {
                return;
            }
            assert(fixture::open(*host, b));
            phase = 2;
            return;
        case 2:
            if (!project || project->project().name != "B")
            {
                return;
            }
            assert(counts["A"].panes == 1 && counts["A"].services == 1 && !delivered);
            old_updates = counts["A"].updates;
            global_at_b = global->updates;
            phase = 3;
            return;
        case 3:
            assert(counts["A"].updates == old_updates && !delivered);
            if (global->updates < global_at_b + 5)
            {
                return;
            }
            release.release();
            assert(fixture::open(*host, c));
            phase = 4;
            return;
        case 4:
            if (!delivered || facts.failed == 0)
            {
                return;
            }
            assert(project && project->project().name == "B");
            assert(counts["C"].panes == 1 && counts["C"].services == 1);
            assert(fixture::open(*host, directory / "missing.luxproj"));
            phase = 5;
            return;
        case 5:
            if (facts.failed != 2)
            {
                return;
            }
            assert(project && project->project().name == "B");
            {
                revision = window_identity->metrics().revision;
                auto state = window_identity->state();
                assert(state);
                state->placement.normal.width = 720;
                state->placement.normal.height = 520;
                assert(window_identity->applyPlacement(state->placement));
            }
            phase = 6;
            return;
        case 6:
            if (window_identity->metrics().revision == revision || counts["B"].draws < 5)
            {
                return;
            }
            assert(window_identity->metrics().width == 720 && window_identity->metrics().height == 520);
            revision = window_identity->metrics().revision;
            draws_before = counts["B"].draws;
            // Arm the real timer BEFORE minimization; a quiet minimized window is
            // allowed to wait indefinitely and need not provide a spare maintenance frame.
            after(
                100ms,
                [&]() noexcept
                {
                    assert(window_identity->metrics().minimized && window_identity->metrics().revision > revision);
                    draws_before = counts["B"].draws;
                    after(
                        100ms,
                        [&]() noexcept
                        {
                            assert(counts["B"].draws == draws_before);
                            std::fprintf(stderr, "Restoring native output after timer\n");
#if defined(_WIN32)
                            ShowWindow(static_cast<HWND>(window_identity->nativeHandle()), SW_RESTORE);
                            std::fprintf(
                                stderr,
                                "Native restore returned, minimized=%d\n",
                                window_identity->metrics().minimized
                            );
#endif
                        }
                    );
                }
            );
#if defined(_WIN32)
            ShowWindow(static_cast<HWND>(window_identity->nativeHandle()), SW_MINIMIZE);
#endif
            phase = 8;
            return;
        case 8:
            if (window_identity->metrics().minimized || counts["B"].draws <= draws_before)
                return;
            assert(engine_identity->renderContext()->runtime().statistics().frames > 0);
            fixture::close(*host);
            phase = 9;
            return;
        case 9:
            if (project)
                return;
            assert(counts["B"].panes == 1 && counts["B"].services == 1);
            // Final close races an accepted real Project worker. Host must drain it safely.
            assert(fixture::open(*host, a));
            host->window().exit();
            phase = 10;
            return;
        default:
            return;
        }
    };
    auto owner = std::make_unique<Global>(step);
    global = owner.get();
    auto added = root.addPane(std::move(owner));
    assert(added);
    global_handle = root.paneHandle(added->get());
    assert(fixture::open(*host, a));
    const auto result = host->run();
    if (!result)
    {
        const auto diagnostic = error::format(result.error());
        std::fprintf(stderr, "Public host run failed at phase %u: %s\n", phase, diagnostic.c_str());
    }
    assert(result);
    assert(phase == 10 && !host->project() && facts.failed == 2 && counts["A"].panes == 1);
    assert(object::ObjectRuntime::instance().statistics().pending == 0);
    std::puts("PASS public run/Events: global UI, blocked worker replacement, Root rejection, metrics, GPU and shutdown"
    );
}
