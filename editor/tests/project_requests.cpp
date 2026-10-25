#include "support/ProjectRequests.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <map>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;

namespace
{
    struct Lifetime final
    {
        unsigned panes{}, contexts{}, constructions{};
    };

    struct Service final
    {
        explicit Service(Lifetime& value) : value(value) {}

        ~Service()
        {
            *alive = false;
            ++value.contexts;
        }

        Lifetime& value;
        std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
    };

    class Pane final : public ui::Pane
    {
    public:
        Pane(EditorContext& context, Lifetime& lifetime, bool closed = false)
            : ui::Pane("Project"), context_(context), lifetime_(lifetime)
        {
            auto service = context.service<Service>();
            assert(service);
            service_alive_ = service->get().alive;
            ++lifetime_.constructions;
            if (closed)
            {
                beginDestruction();
            }
        }

        ~Pane() override
        {
            assert(!context_.project().name.empty());
            assert(*service_alive_); // This exact Pane's service/Context outlives it, including C -> C replacement.
            ++lifetime_.panes;
        }

    private:
        EditorContext& context_;
        Lifetime& lifetime_;
        std::shared_ptr<const bool> service_alive_;
    };

    struct Held final
    {
        std::counting_semaphore<2> release{0};
        std::atomic_uint started{}, delivered{};
        std::atomic_bool stopped{};
    };

    void hold(execution::TaskScope& scope, const std::shared_ptr<Held>& held)
    {
        auto scheduler = scope.execution().blocking();
        assert(scheduler);
        assert(scope.submit(
            {"Held operation", "test"},
            [&,
             make_work =
                 [held, scheduler = *scheduler](execution::TaskReporter reporter) noexcept
             {
                 return stdexec::schedule(scheduler) | stdexec::then(
                                                           [held, reporter]() noexcept -> FrameworkResult<void>
                                                           {
                                                               ++held->started;
                                                               held->release.acquire();
                                                               held->stopped = reporter.stopToken().stop_requested();
                                                               return {};
                                                           }
                                                       );
             },
             consume_result =
                 [held](execution::TTaskResult<void, error::Error>&& result) noexcept
             {
                 assert(result);
                 ++held->delivered;
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready = stdexec::continues_on(
                    lux::execution::taskResult(make_work(reporter)),
                    scope.execution().mainScheduler()
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
    }
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    uuids::uuid_name_generator id{*uuids::uuid::from_string("886e80af-1274-4b38-9306-c8709517ffb5")};
    std::map<std::string, Lifetime> lives;
    const auto file = [&](const std::string& name)
    {
        const auto path = directory / (name + ".luxproj");
        assert(writeProjectManifestAtomic(path, {1, id(name), name}, EProjectWrite::REPLACE));
        return path;
    };
    const auto a = file("A"), b = file("B"), c = file("C"), rejected = file("Rejected");
    EditorConfig config{"Project event qualification", 400, 300};
    config.layout = {{"probe", "project", "Project"}};
    LuxEngine* engine{};
    bool request_from_factory{};
    auto assemble = [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
    {
        assert(composition.registerServiceFactory<Service>(
            [&](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<Service>>
            { return std::make_unique<Service>(lives[context.project().name]); }
        ));
        return composition.registerUiFactory(
            "probe",
            [&](EditorContext& context, const PaneDescription&) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
            {
                auto candidate = std::make_unique<Pane>(
                    context,
                    lives[context.project().name],
                    context.project().name == "Rejected"
                );
                if (request_from_factory && context.project().name == "B")
                {
                    request_from_factory = false;
                    assert(fixture::open(*engine, c));
                }
                return candidate;
            }
        );
    };
    config.project_file = a;
    auto host = LuxEngine::create(std::move(config), assemble);
    assert(host);
    engine = host->get();
    auto& root = engine->window().uiRoot();
    fixture::ProjectFacts facts{*engine};
    const auto until = [&](auto ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(lux::editor::detail::LuxEngineTestAccess::pumpOnce(*engine));
            std::this_thread::sleep_for(1ms);
        }
    };
    bool rejected_recursive_step{};
    auto& execution = engine->engine().execution();
    assert(execution.spawn(stdexec::then(
        stdexec::schedule(execution.mainScheduler()),
        [&]() noexcept
        {
            const auto nested = detail::LuxEngineTestAccess::pumpOnce(*engine);
            assert(!nested && nested.error().type == Errors::EditorRecursiveHostFrame);
            assert(engine->project().project().name == "A");
            rejected_recursive_step = true;
        }
    )));
    until([&] { return rejected_recursive_step; });
    const auto idle = [&] { return fixture::preparationsFinished(*engine); };
    const auto open = [&](const auto& path)
    {
        assert(fixture::open(*engine, path));
        until(idle);
    };
    auto global = root.addPane(std::make_unique<ui::Pane>("Global"));
    assert(global);
    const auto global_handle = root.paneHandle(global->get());
    unsigned notifications{};
    auto connection = object::LuxObject::connect(
        &root,
        &ui::Root::paneChanged,
        [&](const ui::PaneChanged& change) noexcept
        {
            ++notifications;
            if (change.attached)
            {
                assert(!engine->project().project().name.empty());
                assert(root.resolvePane(root.paneHandle(*change.pane)) == change.pane);
            }
        }
    );
    assert(connection);
    assert(facts.changed == 0 && facts.failed == 0);
    auto* original = &engine->project();
    ui::PaneHandle original_handle;
    auto find = [&](ui::Pane& pane) noexcept
    {
        if (&pane != &global->get())
        {
            original_handle = root.paneHandle(pane);
        }
    };
    assert(root.forEachPane(find));
    assert(original_handle != global_handle);

    // L07/L08: preparation failure and a candidate rejected by the actual Root transaction are atomic.
    open(directory / "missing.luxproj");
    assert(facts.failed == 1 && facts.changed == 0 && &engine->project() == original);
    assert(root.resolvePane(original_handle));
    open(rejected);
    assert(facts.failed == 2 && facts.changed == 0 && &engine->project() == original);
    until([&] { return lives["Rejected"].contexts == 1; }); // Finite cleanup, independent of the rejection fact.
    assert(root.resolvePane(original_handle) && lives["Rejected"].panes == 1 && lives["Rejected"].contexts == 1);

    // L05: occupy the real blocking scheduler, admit B then C, and let both accepted requests settle.
    auto blockers = std::make_unique<execution::TaskScope>(engine->engine().execution());
    auto held = std::make_shared<Held>();
    hold(*blockers, held);
    hold(*blockers, held);
    until([&] { return held->started == 2; });
    assert(fixture::open(*engine, b));
    assert(fixture::open(*engine, c));
    assert(&engine->project() == original);
    held->release.release(2);
    until(idle);
    assert(engine->project().project().name == "C" && facts.changed == 1 && facts.failed == 2);
    assert(lives["B"].constructions == 0 && lives["A"].contexts == 1);
    until([&] { return held->delivered == 2; });
    blockers.reset();

    // L06: a later accepted intent supersedes B even when its Worker has queued a completion.
    const auto changed_before = facts.changed;
    const auto b_before = lives["B"].constructions;
    assert(fixture::open(*engine, b));
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!engine->engine().execution().hasReadyWork())
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    assert(fixture::open(*engine, c));
    until(idle);
    assert(facts.changed == changed_before + 1 && facts.failed == 2);
    assert(lives["B"].constructions == b_before);
    assert(engine->project().project().name == "C");
    request_from_factory = true;
    open(b);
    assert(!request_from_factory && engine->project().project().name == "C" && facts.failed == 2);

    // L09: replacement retires C while its accepted worker is still blocked; the loaded session remains B.
    held = std::make_shared<Held>();
    hold(engine->project().tasks(), held);
    until([&] { return held->started == 1; });
    const auto contexts = lives["C"].contexts;
    open(b);
    assert(lives["C"].contexts == contexts + 1 && held->delivered == 0 && root.resolvePane(global_handle));
    held->release.release();
    until([&] { return held->delivered == 1; });
    assert(held->stopped);

    // L10: real CREATE publication survives a later plugin failure.
    const auto create_directory =
        directory / ("Published-" +
                     uuids::to_string(id(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))));
    ProjectManifest broken{1, id("published"), "Published", {{"Missing.Plugin-1", 1}}};
    assert(fixture::create(*engine, {create_directory, broken}));
    until(idle);
    assert(facts.failed == 3 && facts.failure.manifest_published && engine->project().project().name == "B");
    assert(readProjectManifest(create_directory / "Project.luxproj"));

    // L11: a removed and re-registered Pane belongs to the external mount, not the old ProjectUiMount.
    open(a);
    assert(root.forEachPane(find));
    auto removed = root.removePane(*root.resolvePane(original_handle));
    assert(removed);
    // This fixture's Pane borrows Context. Destroy it, then reuse the released registration for independent UI.
    removed->reset();
    auto reused = root.addPane(std::make_unique<ui::Pane>("External"));
    assert(reused);
    const auto reused_handle = root.paneHandle(reused->get());
    open(b);
    assert(root.resolvePane(reused_handle) && !root.resolvePane(original_handle));
    assert(root.resolvePane(global_handle));
    connection->disconnect();

    // L12: the real Root forwards an unfocused shortcut to LuxEngine, not a test dispatcher.
    open(a);
    assert(root.clearPanes());
    assert(root.setMenu({{{ui::CommandId{"lux.project.close"}, "Close project", "Ctrl+W", {ui::EKey::W, true}}}, {}}));
    ui::DrawData draw;
    for (unsigned i{}; i != 3; ++i)
    {
        assert(root.update({{400, 300}, 0.016F}, draw));
    }
    assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
    assert(root.feedInput(ui::Key{ui::EKey::W, true}));
    assert(root.update({{400, 300}, 0.016F}, draw));
    assert(root.update());
    assert(!engine->project().project().name.empty()
    ); // Event accepted; structural change waits for the host safe point.
    assert(engine->run());
    assert(!root.resolvePane(global_handle) && !root.resolvePane(reused_handle));

    // L13 uses a new loaded session: CloseProjectRequest no longer returns to an empty editor.
    host->reset();
    EditorConfig second_config{"Close on adoption", 400, 300};
    second_config.project_file = a;
    second_config.layout = {{"probe", "project", "Project"}};
    host = LuxEngine::create(std::move(second_config), assemble);
    assert(host);
    engine = host->get();
    auto& second_root = engine->window().uiRoot();
    bool closed_on_adoption{};
    auto close_on_add = object::LuxObject::connect(
        &second_root,
        &ui::Root::paneChanged,
        [&](const ui::PaneChanged& change) noexcept
        {
            if (change.attached)
            {
                assert(engine->project().project().name == "B");
                closed_on_adoption = true;
                fixture::close(*engine);
            }
        }
    );
    assert(close_on_add && fixture::open(*engine, b));
    assert(engine->run() && closed_on_adoption);
    close_on_add->disconnect();
    host->reset();
    assert(notifications != 0);
    std::puts("PASS real Project Events: latest wins, queued stale candidates, Root rollback, immediate close, "
              "publication, generations");
}
