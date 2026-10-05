#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <thread>

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
    std::cout << "Installed ProjectCreation: same allocation, real files, closed UI, conflict, safe retirement\n";
}
