#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <cassert>
#include <iostream>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    class FreePane final : public ui::Pane
    {
    public:
        explicit FreePane(const views::ViewFactoryInput& input)
            : Pane(input.dispatcher(), input.paneId(), ui::PaneTypeId{"ec1.free"}, "Free pane"),
              layout_(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
              label_(layout_, ui::ElementId{"text"}, "No author content or renderer")
        {
            assert(setContent(layout_));
        }

    private:
        ui::Layout layout_;
        ui::Label label_;
    };
} // namespace
int main()
{
    auto messages = take(object::ObjectMessageQueue::create(32));
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    desktop::ViewHost host{*root};
    lux::editor::desktop::EditorContext editor_context{messages.dispatcherRef()};
    auto& commands = editor_context.commands();
    extensions::ContributionRegistry contributions{messages.dispatcherRef(), editor_context};
    extensions::ContributionDraft draft;
    draft.views.push_back(views::ViewFactoryEntry::create(
        lux::object::CodeLease::builtin(),
        views::ViewFactoryDescriptor{views::ViewTypeIdView{"ec1.free"}, "Free pane", cxx::typeToken<std::monostate>()},
        [](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
        { return views::DetachedView{lux::object::CodeLease::builtin(), std::make_unique<FreePane>(input)}; }
    ));
    auto candidate = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
    assert(contributions.enqueue(candidate) && contributions.applyPending());
    views::ViewFactoryInput input{
        messages.dispatcherRef(),
        ui::PaneId{"free"},
        lux::object::CodeLease::builtin(),
        cxx::typeToken<std::monostate>(),
        std::make_shared<const std::monostate>()
    };
    auto detached = take(contributions.snapshot().views().prepare(views::ViewTypeId{"ec1.free"}, input));
    assert(!detached.pane()->attachedRoot());
    const auto id = take(host.adopt(detached, views::ViewRestoreKey{"free"})).id;
    assert(take(host.describe(id)).type == views::ViewTypeId{"ec1.free"});
    assert(host.focus(id) && host.close(id));
    take(host.drain());
    assert(!host.describe(id));
    std::cout << "PASS installed detached controls/registry/host without a concrete editor or renderer\n";
}
