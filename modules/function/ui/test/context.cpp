#include "../../../../cmake/installed-consumers/common/UiDrawPane.hpp"
#include <cassert>
#include <imgui.h>
int main()
{
    using namespace lux;
    auto* original = ImGui::CreateContext();
    auto messages_created = object::ObjectMessageQueue::create(64);
    assert(messages_created);
    auto messages = std::move(*messages_created);
    auto first = ui::Root::create(messages.dispatcherRef());
    auto second = ui::Root::create(messages.dispatcherRef());
    assert(first && second && ImGui::GetCurrentContext() == original);
    ui::DrawData slot;
    {
        TUiDrawPane one(**first, [] { ImGui::TextUnformatted("First root"); });
        TUiDrawPane two(**second, [] { ImGui::TextUnformatted("Second root"); });
        (*first)->deferChange(one, [](object::LuxObject& target) noexcept {
            static_cast<ui::Pane&>(target).setTitle("Adopted");
        });
        assert(one.title() == "Draw");
        (*first)->applyPendingChanges();
        assert(one.title() == "Adopted");
        for (int i{}; i < 100; ++i)
        {
            assert((i % 2 ? *first : *second)->update({{640, 480}, 1.0F / 60.0F}, &slot));
            assert(slot.valid() && ImGui::GetCurrentContext() == original);
        }
    }
    first->reset();
    second->reset();
    assert(slot.valid() && ImGui::GetCurrentContext() == original);
    ImGui::DestroyContext(original);
}
