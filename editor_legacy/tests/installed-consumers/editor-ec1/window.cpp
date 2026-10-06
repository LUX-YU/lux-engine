#include <array>
#include <cassert>
#include <iostream>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>

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
        explicit FreePane(const desktop::UiCreateInfo& input)
            : Pane(input.dispatcher, input.instance, ui::PaneTypeId{"ec1.free"}, "Free pane"),
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
    lux::editor::desktop::EditorContext editor_context{messages.dispatcherRef()};
    auto& commands = editor_context.commands();
    extensions::ContributionRegistry contributions{messages.dispatcherRef(), editor_context};
    extensions::ContributionDraft draft;
    static constexpr desktop::UiDescriptor descriptor{
        .type = views::ViewTypeIdView{"ec1.free"},
        .label = "Free pane",
        .create = [](services::ServiceResolver&, const desktop::UiCreateInfo& input
                  ) -> desktop::UiResult<std::unique_ptr<ui::Pane>> { return std::make_unique<FreePane>(input); }
    };
    draft.ui.push_back(desktop::UiEntry::bind<descriptor>(object::CodeLease::builtin()));
    auto candidate = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
    assert(contributions.enqueue(candidate) && contributions.applyPending());
    desktop::UiCreateInfo input{messages.dispatcherRef(), ui::PaneId{"free"}, {}, {}};
    auto handle = take(contributions.snapshot().ui().find(views::ViewTypeIdView{"ec1.free"}));
    auto detached = take(editor_context.ui().create(handle, editor_context.scope(), input));
    assert(!detached->attachedRoot());
    auto* pane = detached.get();
    assert(root->addSubPane(std::move(detached)));
    const auto id = take(root->identify(*pane));
    assert(take(editor_context.ui().describe(*root)).front().type == views::ViewTypeId{"ec1.free"});
    assert(root->requestFocus(*pane));
    auto close = take(editor_context.ui().prepareClose(*root, std::array{id}));
    assert(root->commit(close) && messages.collectRetired() == 1);
    assert(!root->findPane(id));
    std::cout << "PASS installed detached controls/registry/Root without a concrete editor or renderer\n";
}
