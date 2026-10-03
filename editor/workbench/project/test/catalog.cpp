#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <cassert>
#include <cstdio>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }

}
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    desktop::ViewHost host(*root);
    project::ProjectCatalogModel source(messages.dispatcherRef(), 41);
    unsigned opens{};
    AssetReference opened;
    const asset::AssetId asset{*uuids::uuid::from_string("9bef91a4-0d25-4dff-8245-1c968a3dcce5")};
    assert(source.replace("test", {{asset, {}, 13, "UserPackage/test.asset"}}));
    auto candidate = project::makeProjectView(messages.dispatcherRef(), ui::PaneId{"project"}, source);
    auto* view = static_cast<project::ProjectView*>(candidate.pane());
    auto open =
        take(object::LuxObject::connect(view, &project::ProjectView::openRequested, [&](AssetReference value) noexcept {
            ++opens;
            opened = value;
        }));
    assert(root->panes().empty() && view->catalog().assets().size() == 1);
    const auto id = take(host.adopt(candidate, views::ViewRestoreKey{"project"})).id;
    const auto ref = source.reference(asset);
    assert(view->requestOpen(ref) && opens == 1 && opened.asset == asset);
    const auto original = take(source.snapshot());
    const auto* rows = original.assets().data();
    assert(view->refresh() && view->catalog().assets().data() == rows);
    source.setFailure(project::EProjectQueryError::BUSY);
    assert(!view->refresh() && view->catalog().assets().size() == 1 && view->status());
    assert(!view->requestOpen(ref) && opens == 1);
    source.setFailure(project::EProjectQueryError::IO);
    assert(!view->refresh() && view->catalog().version().revision == 1);
    source.setFailure({});
    assert(source.replace("test", {{asset, {}, 13, "UserPackage/test.asset"}}));
    assert(view->refresh() && view->catalog().version().revision == 2);
    assert(!view->requestOpen(ref) && opens == 1);
    ui::Pane pane(messages.dispatcherRef(), ui::PaneId{"picker"}, ui::PaneTypeId{"picker"}, "Picker");
    ui::Layout layout(pane, ui::ElementId{"layout"});
    project::AssetPickerElement picker(layout, ui::ElementId{"asset"}, &source, 13);
    unsigned edits{};
    auto connection = take(object::LuxObject::connect(
        &picker,
        &project::AssetPickerElement::edited,
        [&](ui::EditResult result) noexcept {
            assert(result.changed && result.committed);
            ++edits;
        }
    ));
    picker.setValue(asset);
    assert(edits == 0);
    picker.setValue({});
    assert(picker.select(source.reference(asset)) && edits == 1);
    assert(!picker.select(ref) && picker.value() == asset && edits == 1);
    source.setFailure(project::EProjectQueryError::PERMISSION);
    assert(!picker.refresh() && picker.value() == asset);
    // Failed replacement is atomic; old snapshots own their rows through replacement and view destruction.
    const auto current = source.revision();
    assert(!source.replace("duplicate", {{asset, {}, 1, "a"}, {asset, {}, 2, "b"}}));
    assert(source.revision() == current && original.assets().data() == rows && original.assets()[0].magic == 13);
    source.setFailure({});
    for (int i{}; i != 1000; ++i)
        assert(view->refresh() && view->catalog().assets().data() == source.entries().data());
    auto other = project::makeProjectView(messages.dispatcherRef(), ui::PaneId{"project-second"}, source);
    auto* second = static_cast<project::ProjectView*>(other.pane());
    const auto second_id = take(host.adopt(other, views::ViewRestoreKey{"project-second"})).id;
    assert(second->catalog().assets().data() == view->catalog().assets().data());
    auto queue = take(object::ObjectMessageQueue::create(1));
    object::LuxObject receiver(queue.dispatcherRef());
    unsigned notified{};
    auto queued = take(object::LuxObject::connect(
        &source,
        &project::ProjectCatalogModel::changed,
        &receiver,
        [&](std::uint64_t) noexcept { ++notified; },
        object::EDelivery::QUEUED
    ));
    auto notice = source.dispatchChanges();
    assert(notice.queued == 1 && notice.direct >= 2);
    assert(source.replace("next", {{asset, {}, 13, "NewPackage/test.asset"}}));
    notice = source.dispatchChanges();
    assert(notice.full == 1 && notified == 0);
    assert(root->update({{640, 480}, .016F}, nullptr));
    assert(second->catalog().assets().data() == view->catalog().assets().data());
    assert(view->catalog().assets().front().path == "NewPackage/test.asset");
    assert(original.assets().front().path == "UserPackage/test.asset");
    assert(original.find(asset) == &original.assets().front());
    assert(view->catalog().find(asset) == &view->catalog().assets().front());
    assert(original.find(asset) != view->catalog().find(asset));
    assert(original.find(asset)->path == "UserPackage/test.asset");
    assert(!original.find({}));
    assert(queue.dispatchPending() == 1 && notified == 1);
    queue.close();
    source.setFailure(project::EProjectQueryError::BUSY);
    assert(source.dispatchChanges().closed == 1);
    assert(!view->refresh() && view->catalog().assets().size() == 1);
    source.setFailure({}); // Deliberately omit notification: cheap revision/status resync must recover.
    assert(root->update({{640, 480}, .016F}, nullptr));
    assert(!view->status());
    auto copied = original;
    auto moved = std::move(copied);
    assert(copied.assets().empty() && copied.name().empty() && moved.assets().data() == rows);
    assert(copied.version() == project::ProjectCatalogVersion{} && !copied.find(asset));
    assert(moved.find(asset) == rows);
    project::ProjectCatalogModel next_project(messages.dispatcherRef(), 42);
    assert(next_project.replace("other project", {{asset, {}, 13, "a"}}));
    assert(!next_project.resolve(source.reference(asset), 13));
    assert(host.close(second_id) && host.drain());
    assert(host.close(id) && host.drain());
    assert(source.entries().size() == 1 && opens == 1);
    // Delivery failure is not open admission; a partial broadcast must not be replayed.
    project::ProjectView intents(messages.dispatcherRef(), ui::PaneId{"intents"}, source);
    auto intent_queue = take(object::ObjectMessageQueue::create(1));
    object::LuxObject intent_receiver(intent_queue.dispatcherRef());
    unsigned synchronous{}, asynchronous{};
    auto direct_intent =
        take(object::LuxObject::connect(&intents, &project::ProjectView::openRequested, [&](AssetReference) noexcept {
            ++synchronous;
        }));
    auto delayed_intent = take(object::LuxObject::connect(
        &intents,
        &project::ProjectView::openRequested,
        &intent_receiver,
        [&](AssetReference) noexcept { ++asynchronous; },
        object::EDelivery::QUEUED
    ));
    const auto current_reference = source.reference(asset);
    assert(intents.requestOpen(current_reference));
    auto rejected_intent = intents.requestOpen(current_reference);
    assert(!rejected_intent && synchronous == 2 && asynchronous == 0);
    assert(std::get<project::EProjectQueryError>(rejected_intent.error()) == project::EProjectQueryError::CAPACITY);
    assert(intent_queue.dispatchPending() == 1 && asynchronous == 1 && synchronous == 2);
    intent_queue.close();
    rejected_intent = intents.requestOpen(current_reference);
    assert(!rejected_intent && synchronous == 3 && asynchronous == 1);
    assert(std::get<project::EProjectQueryError>(rejected_intent.error()) == project::EProjectQueryError::CLOSED);
    // Production descriptor binding: no Application, and each detached owner owns its own connection.
    unsigned received{};
    auto lifetime = std::make_shared<int>(17);
    std::weak_ptr<int> weak = lifetime;
    auto factory = project::makeProjectViewFactory(source, [pin = lifetime, &received](const AssetReference&) {
        assert(*pin == 17);
        ++received;
    });
    lifetime.reset();
    auto peer_factory = project::makeProjectViewFactory(source, {});
    assert(&factory->descriptor() == &peer_factory->descriptor());
    auto factories = take(views::ViewFactorySnapshot::create({factory}));
    const auto build = [&](const char* name) {
        return take(factories.prepare(views::ViewTypeId{"lux.editor.project"}, {
            messages.dispatcherRef(), ui::PaneId{name}, contracts::CodeLease::builtin(),
            cxx::typeToken<std::monostate>(), std::make_shared<const std::monostate>()
        }));
    };
    const auto mounted_before = root->panes().size();
    auto first_bound = build("factory-first");
    auto second_bound = build("factory-second");
    std::printf("Detached tool factories: Root windows before=%zu after=%zu\n", mounted_before, root->panes().size());
    assert(root->panes().size() == mounted_before);
    auto* first_pane = static_cast<project::ProjectView*>(first_bound.pane());
    auto* second_pane = static_cast<project::ProjectView*>(second_bound.pane());
    const auto first_id = take(host.adopt(first_bound, views::ViewRestoreKey{"factory-first"})).id;
    const auto last_id = take(host.adopt(second_bound, views::ViewRestoreKey{"factory-second"})).id;
    factory.reset();
    factories = {};
    assert(!weak.expired());
    assert(first_pane->requestOpen(current_reference) && received == 1);
    assert(host.close(first_id) && host.drain());
    assert(!weak.expired() && second_pane->requestOpen(current_reference) && received == 2);
    assert(host.close(last_id) && host.drain());
    assert(weak.expired());
}
