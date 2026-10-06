#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

static_assert(!std::is_copy_constructible_v<lux::editor::ProjectLaunching>);
static_assert(!std::is_copy_assignable_v<lux::editor::ProjectLaunching>);
static_assert(!std::is_move_constructible_v<lux::editor::ProjectLaunching>);
static_assert(!std::is_move_assignable_v<lux::editor::ProjectLaunching>);

template <class Result> auto take(Result&& value)
{
    assert(value);
    return std::move(*value);
}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 3);
    if (std::string_view{argv[1]} == "--project")
    {
        std::ofstream received(argv[2]);
        received << "actual process received project argument";
        return received ? 0 : 3;
    }
    const auto directory = std::filesystem::absolute(argv[2]);
    // This isolated consumer owns only its exact test directory.
    assert(directory.filename() == "creation" && directory.has_parent_path());
    std::filesystem::remove_all(directory);
    auto execution =
        take(process::ExecutionRuntime::create({2, 64, 64, {64}, process::BlockingSchedulerConfig{2, 32}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    lux::editor::project::ProjectCreationOptions options{argv[1], false};
    services::ServiceRegistry registry{messages.dispatcherRef()};
    auto scope = take(registry.createScope());
    assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, execution));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.creation.options"}, options));
    assert(registry.publish(
        {services::ServiceEntry::bind<lux::editor::project::kProjectCreationService>(object::CodeLease::builtin())}
    ));
    auto owner = take(registry.get<lux::editor::project::ProjectCreation>(scope));
    auto second = take(registry.get<lux::editor::project::ProjectCreation>(scope));
    assert(owner == second && !owner->progress().pending && !owner->catalog());
    auto settle = [&]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (owner->progress().pending)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution.collectCompletions());
            assert(scope.maintain());
            std::this_thread::yield();
        }
    };
    assert(owner->start());
    settle();
    assert(owner->catalog() && !owner->progress().failure);
    assert(owner->select({}));
    settle();
    assert(!owner->progress().failure);
    assert(owner->configuration());
    desktop::UiRegistry windows{messages.dispatcherRef(), registry};
    auto catalog = take(desktop::UiCatalog::prepare(
        {desktop::UiEntry::bind<lux::editor::project::kProjectCreationView>(object::CodeLease::builtin())}
    ));
    assert(windows.publish(catalog));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    const auto factory = take(catalog.find(lux::editor::project::kProjectCreationView.type));
    assert(windows.mount(*root, scope, {{factory, {messages.dispatcherRef(), ui::PaneId{"creation"}, {}, {}}}}));
    assert(owner->create({directory, "SDK creation", "", {}}));
    const auto window = take(windows.describe(*root)).front().handle;
    auto closing = take(windows.prepareClose(*root, std::span{&window, 1}));
    assert(root->commit(closing));
    assert(messages.collectRetired() == 1);
    assert(owner->progress().pending && !owner->progress().committed);
    settle();
    const auto& progress = owner->progress();
    assert(progress.committed && !progress.failure && !progress.launched);
    assert(prepareProjectOpen(progress.committed->project_file));
    assert(std::filesystem::exists(directory / "Project.luxproject"));
    assert(!std::filesystem::exists(directory / "Content"));
    assert(&second->progress() == &progress);
    // Published facts remain until explicit reset. A second attempt cannot replace an existing project.
    assert(owner->beginNew());
    assert(owner->create({directory, "Rejected overwrite", "", {}}));
    settle();
    assert(owner->progress().failure && !owner->progress().committed);
    assert(take(prepareProjectOpen(directory / "Project.luxproject")).manifest().name == "SDK creation");
    assert(take(scope.settled()));
    std::weak_ptr<lux::editor::project::ProjectCreation> lifetime = owner;
    owner.reset();
    second.reset();
    assert(!lifetime.expired());
    assert(scope.release());
    assert(lifetime.expired() && !scope.drained());
    assert(messages.collectRetired() == 1 && scope.drained() && registry.drained());

    // Actual public asynchronous launching, including a failed installation and later real child process.
    // The parent uses only SDK headers; the child witnesses the existing platform launch arguments.
    {
        auto launch_scope = take(registry.createScope());
        auto installation = directory / "installation with spaces";
        assert(launch_scope.provide(services::ServiceNameView{"lux.process.execution"}, execution));
        assert(launch_scope.provide(services::ServiceNameView{"lux.editor.installation"}, installation));
        assert(registry.publish({services::ServiceEntry::bind<kProjectLaunchingService>(object::CodeLease::builtin())})
        );
        auto launching = take(registry.get<ProjectLaunching>(launch_scope));
        assert(launching == take(registry.get<ProjectLaunching>(launch_scope)));
        const auto received = directory / "received-project.txt";
        auto settle_launch = [&]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (launching->pending())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                assert(execution.collectCompletions());
                std::this_thread::yield();
            }
        };
        assert(launching->request(received));
        auto full = launching->request(received);
        assert(!full && full.error().code == EEditorError::BUSY);
        auto unfinished = launching->acknowledge();
        assert(!unfinished && unfinished.error().code == EEditorError::BUSY);
        settle_launch();
        assert(launching->result() && !*launching->result());
        assert(launching->result()->error().domain == "editor.launch");
        const auto retained_failure = launching->result()->error();
        full = launching->request(received);
        assert(!full && full.error().code == EEditorError::BUSY);
        assert(launching->result()->error().reason == retained_failure.reason);
        assert(launching->acknowledge());
        std::filesystem::create_directories(installation / "bin");
        const auto executable = std::filesystem::absolute(argv[0]);
        auto child = installation / "bin/lux_editor";
        child += executable.extension();
        std::filesystem::copy_file(executable, child);
        assert(launching->request(received));
        settle_launch();
        assert(launching->result() && *launching->result());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string argument;
        while (argument.empty())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::ifstream input(received);
            std::getline(input, argument);
            std::this_thread::yield();
        }
        assert(argument == "actual process received project argument");
        assert(launching->acknowledge());
        std::thread foreign(
            [&]()
            {
                auto rejected = launching->request(received);
                assert(!rejected && rejected.error().code == EEditorError::INVALID_STATE);
            }
        );
        foreign.join();
        assert(!launching->pending() && !launching->result());
        launching.reset();
        assert(launch_scope.release());
        assert(messages.collectRetired() == 1 && launch_scope.drained() && registry.drained());
    }
    std::cout << "Installed ProjectCreation: same allocation, real files, closed UI, conflict, safe retirement\n";
}
