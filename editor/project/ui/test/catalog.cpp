#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <cassert>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    struct Catalog final
    {
        project::ProjectCatalog value{{41, 1}, "test", {}};
        std::optional<project::EProjectQueryError> failure;
        unsigned reads{}, opens{};
        AssetReference opened;
        project::ProjectCatalogAccess access()
        {
            return {
                this,
                +[](const void* pointer) -> project::ProjectQueryResult<project::ProjectCatalogVersion> {
                    const auto& self = *static_cast<const Catalog*>(pointer);
                    if (self.failure)
                        return cxx::unexpected(project::VProjectQueryFailure{*self.failure});
                    return self.value.version;
                },
                +[](const void* pointer) -> project::ProjectQueryResult<project::ProjectCatalog> {
                    auto& self = *const_cast<Catalog*>(static_cast<const Catalog*>(pointer));
                    ++self.reads;
                    if (self.failure)
                        return cxx::unexpected(project::VProjectQueryFailure{*self.failure});
                    return self.value;
                },
                +[](const void* pointer, AssetReference reference, std::uint32_t magic
                 ) -> project::ProjectQueryResult<asset::AssetId> {
                    const auto& self = *static_cast<const Catalog*>(pointer);
                    if (self.failure)
                        return cxx::unexpected(project::VProjectQueryFailure{*self.failure});
                    if (reference.catalog_revision != self.value.version.revision)
                        return cxx::unexpected(project::VProjectQueryFailure{EAssetReferenceError::STALE_CATALOG});
                    if (reference.project_instance != self.value.version.instance)
                        return cxx::unexpected(project::VProjectQueryFailure{EAssetReferenceError::FOREIGN_PROJECT});
                    for (const auto& entry : self.value.assets)
                        if (entry.id == reference.asset)
                        {
                            if (magic && entry.magic != magic)
                                return cxx::unexpected(project::VProjectQueryFailure{EAssetReferenceError::WRONG_TYPE});
                            return entry.id;
                        }
                    return cxx::unexpected(project::VProjectQueryFailure{EAssetReferenceError::MISSING_ASSET});
                }
            };
        }
        project::AssetOpenRequests requests()
        {
            return {this, +[](void* pointer, AssetReference ref) -> project::ProjectQueryResult<void> {
                        auto& self = *static_cast<Catalog*>(pointer);
                        self.opened = ref;
                        ++self.opens;
                        return {};
                    }};
        }
    };
}
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    desktop::ViewHost host(*root);
    Catalog source;
    const asset::AssetId asset{*uuids::uuid::from_string("9bef91a4-0d25-4dff-8245-1c968a3dcce5")};
    source.value.assets.push_back({asset, {}, 13, "UserPackage/test.asset"});
    auto candidate =
        project::makeProjectView(messages.dispatcherRef(), ui::PaneId{"project"}, source.access(), source.requests());
    auto* view = static_cast<project::ProjectView*>(candidate.pane());
    assert(root->panes().empty() && view->catalog().assets.size() == 1);
    const auto id = take(host.adopt(candidate, views::ViewRestoreKey{"project"})).id;
    const auto ref = source.value.reference(asset);
    assert(view->requestOpen(ref) && source.opens == 1 && source.opened.asset == asset);
    const auto reads = source.reads;
    assert(view->refresh() && source.reads == reads);
    source.failure = project::EProjectQueryError::BUSY;
    assert(!view->refresh() && view->catalog().assets.size() == 1 && view->status());
    assert(!view->requestOpen(ref) && source.opens == 1);
    source.failure = project::EProjectQueryError::IO;
    assert(!view->refresh() && view->catalog().version.revision == 1);
    source.failure.reset();
    ++source.value.version.revision;
    assert(view->refresh() && view->catalog().version.revision == 2);
    assert(!view->requestOpen(ref) && source.opens == 1);
    ui::Pane pane(messages.dispatcherRef(), ui::PaneId{"picker"}, ui::PaneTypeId{"picker"}, "Picker");
    ui::Layout layout(pane, ui::ElementId{"layout"});
    project::AssetPickerElement picker(layout, ui::ElementId{"asset"}, source.access(), 13);
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
    assert(picker.select(source.value.reference(asset)) && edits == 1);
    assert(!picker.select(ref) && picker.value() == asset && edits == 1);
    source.failure = project::EProjectQueryError::PERMISSION;
    assert(!picker.refresh() && picker.value() == asset);
    assert(host.close(id) && host.drain());
    assert(source.value.assets.size() == 1 && source.opens == 1);
}
