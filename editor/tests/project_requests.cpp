#include "support/ProjectRequests.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/process/TaskScope.hpp>
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
    void hold(process::TaskScope& scope, const std::shared_ptr<Held>& held)
    {
        auto scheduler = scope.execution().blocking();
        assert(scheduler);
        assert(scope.submit(
            {"Held operation", "test"},
            [held, scheduler = *scheduler](process::TaskReporter reporter) noexcept
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
            [held](process::TTaskResult<void, error::Error>&& result) noexcept
            {
                assert(result);
                ++held->delivered;
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
    auto host = LuxEngine::create(
        std::move(config),
        [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            assert(composition.registerServiceFactory<Service>(
                [&](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<Service>>
                { return std::make_unique<Service>(lives[context.project().name]); }
            ));
            return composition.registerUiFactory(
                "probe",
                [&](EditorContext& context,
                    const PaneDescription&) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
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
        }
    );
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
            assert(engine->frame());
            std::this_thread::sleep_for(1ms);
        }
    };
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
                assert(engine->project());
                assert(root.resolvePane(root.paneHandle(*change.pane)) == change.pane);
            }
        }
    );
    assert(connection);
    open(a);
    assert(facts.changed == 1 && facts.failed == 0);
    auto* original = engine->project();
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
    assert(facts.failed == 1 && facts.changed == 1 && engine->project() == original);
    assert(root.resolvePane(original_handle));
    open(rejected);
    assert(facts.failed == 2 && facts.changed == 1 && engine->project() == original);
    assert(root.resolvePane(original_handle) && lives["Rejected"].panes == 1 && lives["Rejected"].contexts == 1);

    // L05: occupy the real blocking scheduler, admit B then C, and let both accepted requests settle.
    auto blockers = std::make_unique<process::TaskScope>(engine->engine().execution());
    auto held = std::make_shared<Held>();
    hold(*blockers, held);
    hold(*blockers, held);
    until([&] { return held->started == 2; });
    assert(fixture::open(*engine, b));
    assert(fixture::open(*engine, c));
    assert(engine->project() == original);
    held->release.release(2);
    until(idle);
    assert(engine->project()->project().name == "C" && facts.changed == 2 && facts.failed == 2);
    assert(lives["B"].constructions == 0 && lives["A"].contexts == 1);
    until([&] { return held->delivered == 2; });
    blockers.reset();

    // L06: B's actual ObjectScheduler completion and a later C intent share the owner batch.
    const auto changed_before = facts.changed;
    const auto b_before = lives["B"].constructions;
    assert(fixture::open(*engine, b));
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (object::ObjectRuntime::instance().statistics().pending == 0)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    assert(
        object::post(
            engine->target(),
            [c](object::LuxObject* receiver) noexcept
            {
                assert(receiver);
                OpenProjectRequest request{c};
                assert(object::sendEvent(*receiver, request) && !request.rejection.type);
            }
        ) == object::EObjectPostStatus::POSTED
    );
    until(idle);
    assert(facts.changed == changed_before + 1 && facts.failed == 2);
    assert(lives["B"].constructions == b_before + 1 && lives["B"].panes == lives["B"].constructions);
    assert(engine->project()->project().name == "C");
    request_from_factory = true;
    open(b);
    assert(!request_from_factory && engine->project()->project().name == "C" && facts.failed == 2);

    // L09: close destroys the Context immediately even though the accepted worker is still blocked.
    held = std::make_shared<Held>();
    hold(engine->project()->tasks(), held);
    until([&] { return held->started == 1; });
    const auto contexts = lives["C"].contexts;
    fixture::close(*engine);
    until([&] { return !engine->project(); });
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
    assert(facts.failed == 3 && facts.failure.manifest_published && !engine->project());
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
    fixture::close(*engine);
    until([&] { return !engine->project(); });
    assert(root.resolvePane(reused_handle) && !root.resolvePane(original_handle));
    assert(root.resolvePane(global_handle));
    connection->disconnect();

    // L12: the real Root forwards an unfocused shortcut to LuxEngine, not a test dispatcher.
    open(a);
    assert(root.clearPanes());
    root.setMenu({{ui::CommandId{"lux.project.close"}, "Close project", "Ctrl+W", {ui::EKey::W, true}}});
    ui::DrawData draw;
    for (unsigned i{}; i != 3; ++i)
    {
        assert(root.update({{400, 300}, 0.016F}, draw));
    }
    assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
    assert(root.feedInput(ui::Key{ui::EKey::W, true}));
    assert(root.update({{400, 300}, 0.016F}, draw));
    assert(root.update());
    assert(engine->project()); // Event accepted; structural change waits for the host safe point.
    until([&] { return !engine->project(); });
    root.setMenu({});

    // L13: a notification sees B already current; its close request is retained for the next batch.
    const auto changed = facts.changed;
    auto close_on_add = object::LuxObject::connect(
        &root,
        &ui::Root::paneChanged,
        [&](const ui::PaneChanged& change) noexcept
        {
            if (change.attached)
            {
                assert(engine->project() && engine->project()->project().name == "B");
                fixture::close(*engine);
            }
        }
    );
    assert(close_on_add && fixture::open(*engine, b));
    until([&] { return facts.changed == changed + 2; });
    assert(!engine->project());
    close_on_add->disconnect();
    host->reset();
    assert(notifications != 0);
    std::puts("PASS real Project Events: latest wins, queued stale candidates, Root rollback, immediate close, "
              "publication, generations");
}
