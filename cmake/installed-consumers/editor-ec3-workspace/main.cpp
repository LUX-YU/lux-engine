#include <cassert>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/WorkspaceActions.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace w = lux::editor::workspace;
namespace p = lux::editor::persistence;
namespace
{
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
    w::DockLayout layout(std::string id, bool visible)
    {
        w::DockLayout value;
        value.id = {std::move(id)};
        value.label = "Independent workspace";
        value.slots = {{{1}, views::ViewRestoreKey{"one"}, views::ViewTypeId{"test.workspace"}, visible}};
        value.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, 0.5, {{1}}}};
        value.dock.roots = {{1}};
        return value;
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]) /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    storage::FileArtifactStore files{directory};
    p::WriteCoordinator writes;
    w::WorkspaceStore store{directory, writes, files};
    w::WorkspaceChanges changes{store, writes, files};
    auto messages = take(object::ObjectMessageQueue::create(32));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    desktop::EditorContext context{messages.dispatcherRef()};
    auto& windows = context.ui();
    desktop::WorkspaceActions actions{*root, windows, context.scope(), store, changes};
    unsigned constructions{};
    assert(context.scope().provide(services::ServiceNameView{"test.actions"}, actions));
    assert(context.scope().provide(services::ServiceNameView{"test.constructions"}, constructions));
    constexpr services::ServiceDependency dependencies[]{
        {services::ServiceNameView{"test.actions"},
         1,
         cxx::typeToken<desktop::WorkspaceActions>(),
         services::EDependencyKind::BORROWED},
        {services::ServiceNameView{"test.constructions"},
         1,
         cxx::typeToken<unsigned>(),
         services::EDependencyKind::BORROWED}
    };
    desktop::UiDescriptor descriptor{views::ViewTypeIdView{"test.workspace"}, "Test", dependencies};
    descriptor.create = [](services::ServiceResolver& resolver,
                           const desktop::UiCreateInfo& input) -> desktop::UiResult<std::unique_ptr<ui::Pane>>
    {
        auto actions = resolver.require<desktop::WorkspaceActions>(0);
        auto count = resolver.require<unsigned>(1);
        assert(actions && count);
        const auto nested = actions->get().save("reentrant");
        assert(!nested && nested.error().code == EEditorError::BUSY);
        ++count->get();
        return std::make_unique<ui::Pane>(
            input.dispatcher,
            input.instance,
            ui::PaneTypeId{"test.workspace"},
            "Independent view"
        );
    };
    auto catalog =
        take(desktop::UiCatalog::prepare({desktop::UiEntry::create(object::CodeLease::builtin(), descriptor)}));
    assert(windows.publish(catalog));
    auto publish = [&]
    {
        while (auto work = take(writes.takeReady()))
        {
            assert(writes.complete(work->ticket, files.publish(*work)));
        }
        assert(changes.update());
        assert(changes.settled() && writes.size() == 0);
    };
    const auto first = layout("1234567890abcdef1234567890abcdef", true);
    const auto second = layout("abcdef1234567890abcdef1234567890", false);
    assert(changes.save(first) && changes.save(second));
    publish();
    assert(changes.catalog().layouts.size() == 2);
    assert(actions.apply(first.id));
    publish();
    assert(constructions == 1 && take(store.readPreferences()).value.selected_layout == first.id);
    const auto view = take(windows.describe(*root)).front().handle;
    assert(take(root->findPane(view))->visible());
    const auto borrowed = [&](ui::Pane&)
    {
        const auto refused = actions.apply(first.id);
        assert(!refused && refused.error().code == EEditorError::BUSY);
    };
    assert(root->withPane(view, borrowed));
    assert(changes.settled() && writes.size() == 0);

    // Malformed preferences cannot be treated as missing. The already committed Root layout is retained.
    const auto preferences = directory / ".lux/workspace/preferences.toml";
    const std::string malformed = "version = 999\n";
    {
        std::ofstream output(preferences);
        output << malformed;
    }
    const auto applied = actions.apply(second.id);
    assert(!applied && !take(root->findPane(view))->visible() && constructions == 1);
    assert(writes.size() == 0);
    std::ifstream input(preferences);
    assert(std::string(std::istreambuf_iterator<char>{input}, {}) == malformed);
    input.close();

    assert(changes.rename(first.id, "Renamed without changing identity"));
    const auto ticket = changes.publications().back().ticket;
    const auto work = take(writes.takeReady());
    assert(work && work->ticket == ticket);
    assert(std::holds_alternative<p::CommitReceipt>(files.publish(*work)));
    assert(writes.complete(ticket, p::PublicationUnknown{{p::EPersistenceError::IO, "lost receipt"}, "published"}));
    assert(changes.update() && !changes.settled() && !changes.acknowledge(ticket));
    assert(writes.size() == 1 && !changes.publications().back().result);
    assert(changes.reconcile(ticket) && changes.update());
    assert(changes.settled() && writes.size() == 0);
    assert(std::holds_alternative<p::CommitReceipt>(*changes.publications().back().result));
    assert(take(store.readLayout(first.id)).value.label == "Renamed without changing identity");
    assert(changes.acknowledge(ticket));
    bool wrong_thread{};
    std::jthread(
        [&]
        {
            const auto refused = changes.refresh();
            wrong_thread = !refused && refused.error().domain == "workspace.owner-thread";
        }
    ).join();
    assert(wrong_thread);
    std::cout << "PASS installed WorkspaceChanges/Actions, real Root and files, reentrancy, separate UI/disk facts, "
                 "Unknown\n";
}
