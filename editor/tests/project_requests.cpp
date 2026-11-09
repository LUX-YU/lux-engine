#include "support/ProjectRequests.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
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
    constexpr auto initialization_failure = error::errorId("lux.test.project_initialization");
    constexpr auto cleanup_failure = error::errorId("lux.test.project_cleanup");

    struct Lifetime final
    {
        unsigned panes{}, contexts{}, constructions{}, initialized{}, recovered{};
        bool reject_initialization{}, reject_close{}, reject_recovery{};
    };

    struct Service final
    {
        explicit Service(Lifetime& value) : value(value) {}

        ~Service()
        {
            *alive = false;
            ++value.contexts;
        }

        exec::task<FrameworkResult<void>> initializeProject() noexcept
        {
            ++value.initialized;
            if (value.reject_initialization)
            {
                co_return cxx::unexpected(error::Error{initialization_failure});
            }
            co_return FrameworkResult<void>{};
        }

        exec::task<FrameworkResult<void>> closeAsync() noexcept
        {
            if (value.reject_close)
            {
                co_return cxx::unexpected(error::Error{cleanup_failure});
            }
            co_return FrameworkResult<void>{};
        }

        exec::task<FrameworkResult<void>> recoverClose() noexcept
        {
            ++value.recovered;
            if (value.reject_recovery)
            {
                co_return cxx::unexpected(error::Error{cleanup_failure});
            }
            value.reject_close = false;
            co_return FrameworkResult<void>{};
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
        auto scheduler = scope.execution().blockingIoScheduler();
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
                 [held](execution::ExecutionResult<void, error::Error>&& result) noexcept
             {
                 assert(result);
                 ++held->delivered;
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready = stdexec::continues_on(
                    lux::execution::asResult(make_work(reporter)),
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
    const error::ErrorDescriptor errors[]{
        {"lux.test.project_initialization", "Test initialization refusal", error::ERecovery::PERMANENT},
        {"lux.test.project_cleanup", "Test cleanup refusal", error::ERecovery::RETRYABLE}
    };
    assert(error::ErrorRegistry::instance().registerTypes(errors));
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
    config.layout = defaultProjectLayout({{"probe", "project", "Project"}});
    EditorApplication* engine{};
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
                    auto busy = fixture::open(*engine, c);
                    assert(!busy && busy.error().type == Errors::ProjectLoadBusy);
                }
                return candidate;
            }
        );
    };
    config.project_file = a;
    auto host = fixture::createLoadedEditor(std::move(config), assemble);
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
            assert(fixture::stepEditor(*engine));
            std::this_thread::sleep_for(1ms);
        }
    };
    bool rejected_recursive_step{};
    auto& execution = engine->engine().execution();
    assert(execution.spawn(stdexec::then(
        stdexec::schedule(execution.mainScheduler()),
        [&]() noexcept
        {
            const auto nested = fixture::stepEditor(*engine);
            assert(
                !nested &&
                nested.error().type == lux::execution::toError(lux::execution::EExecutionError::INVALID_ARGUMENT).type
            );
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
        if (pane.title() == "Project")
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

    // One finite transaction: a second request is BUSY, cancellation drains accepted work once.
    auto blockers = std::make_unique<execution::TaskScope>(engine->engine().execution());
    auto held = std::make_shared<Held>();
    hold(*blockers, held);
    hold(*blockers, held);
    until([&] { return held->started == 2; });
    const auto completed_before = facts.completed;
    assert(fixture::open(*engine, b));
    auto busy = fixture::open(*engine, c);
    assert(!busy && busy.error().type == Errors::ProjectLoadBusy);
    fixture::cancel(*engine);
    assert(&engine->project() == original);
    held->release.release(2);
    until(idle);
    assert(engine->project().project().name == "A" && facts.changed == 0 && facts.failed == 2);
    assert(facts.cancelled == 1 && facts.completed == completed_before + 1);
    assert(lives["B"].constructions == 0);
    until([&] { return held->delivered == 2; });
    blockers.reset();

    // Requests sent by a candidate factory obey the same admission boundary.
    request_from_factory = true;
    open(b);
    assert(!request_from_factory && engine->project().project().name == "B" && facts.failed == 2);
    open(c);
    held = std::make_shared<Held>();
    hold(engine->project().tasks(), held);
    until([&] { return held->started == 1; });
    const auto contexts = lives["C"].contexts;
    const auto previous_stop = engine->project().tasks().stopToken();
    assert(fixture::open(*engine, b));
    until([&] { return engine->project().project().name == "B"; });
    assert(engine->state() == EApplicationState::LOADING_PROJECT);
    assert(lives["C"].contexts == contexts && held->delivered == 0 && root.resolvePane(global_handle));
    until([&] { return previous_stop.stop_requested(); });
    held->release.release();
    until(idle);
    assert(held->delivered == 1 && held->stopped && lives["C"].contexts == contexts + 1);

    // L10: real CREATE publication survives a later plugin failure.
    const auto create_directory =
        directory / ("Published-" +
                     uuids::to_string(id(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))));
    ProjectManifest broken{1, id("published"), "Published", {{"Missing.Plugin-1", 1}}};
    assert(fixture::create(*engine, {create_directory, broken}));
    until(idle);
    assert(
        facts.failed == 3 && facts.failure.manifest_publication == EManifestPublication::PUBLISHED &&
        engine->project().project().name == "B"
    );
    assert(readProjectManifest(create_directory / "Project.luxproj"));

    // Required initialization errors keep B; a missing startup provider is not silently ignored.
    const auto failed_initialization = file("InitializeFailure");
    lives["InitializeFailure"].reject_initialization = true;
    open(failed_initialization);
    assert(facts.failure.error.type == initialization_failure && !facts.failure.project_published);
    assert(engine->ready() && engine->project().project().name == "B");
    ProjectManifest missing_provider{1, id("missing-startup-provider"), "Missing startup provider"};
    const asset::AssetId startup{id("startup-scene")};
    missing_provider.scenes.push_back({startup, "Scene", "Scene.luxscene", "test.scene"});
    missing_provider.startup_scene = startup;
    const auto startup_file = directory / "Startup.luxproj";
    assert(writeProjectManifestAtomic(startup_file, missing_provider, EProjectWrite::REPLACE));
    open(startup_file);
    assert(facts.failure.error.type == Errors::EditorStartupSceneFailure && engine->ready());

    // A published switch can fail cleanup. The old service remains until explicit recovery succeeds.
    const auto close_contexts = lives["B"].contexts;
    lives["B"].reject_close = true;
    lives["B"].reject_recovery = true;
    open(a);
    assert(engine->state() == EApplicationState::PROJECT_CLEANUP_FAILED);
    assert(facts.failure.error.type == cleanup_failure && facts.failure.project_published);
    assert(engine->project().project().name == "A" && lives["B"].contexts == close_contexts);
    const auto terminals = facts.completed;
    auto rejected_load = fixture::open(*engine, b);
    assert(!rejected_load && rejected_load.error().type == Errors::ProjectLoadBusy);
    RetryProjectCleanupRequest retry;
    assert(object::sendEvent(*engine, retry) && !retry.rejection.type);
    until(idle);
    assert(engine->state() == EApplicationState::PROJECT_CLEANUP_FAILED && lives["B"].recovered == 1);
    lives["B"].reject_recovery = false;
    assert(object::sendEvent(*engine, retry) && !retry.rejection.type);
    until(idle);
    assert(engine->ready() && facts.completed == terminals && lives["B"].contexts == close_contexts + 1);

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

    // L12: the real Root forwards an unfocused shortcut to EditorApplication, not a test dispatcher.
    open(a);
    // Keep the application's modal pane owned until its close transaction has finished.
    assert(root.removePane(*root.resolvePane(global_handle)));
    assert(root.removePane(*root.resolvePane(reused_handle)));
    assert(root.setMenu({{{ui::CommandId{"lux.project.close"}, "Close project", "Ctrl+W", {ui::EKey::W, true}}}, {}}));
    ui::DrawView draw;
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
    second_config.layout = defaultProjectLayout({{"probe", "project", "Project"}});
    host = fixture::createLoadedEditor(std::move(second_config), assemble);
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
    std::puts("PASS real Project Events: BUSY/cancel/one terminal, Context cleanup, Root rollback, immediate close, "
              "publication, generations");
}
