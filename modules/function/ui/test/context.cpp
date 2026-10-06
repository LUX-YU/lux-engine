#include "../../../../cmake/installed-consumers/common/UiDrawPane.hpp"
#include "RootAccess.hpp"
#include <cassert>
#include <imgui.h>
int main()
{
    using namespace lux;
    auto* original = ImGui::CreateContext();
    (void)object::ObjectRuntime::instance();
    auto first = ui::Root::create();
    auto second = ui::Root::create();
    assert(first && second && ImGui::GetCurrentContext() == original);
    auto invalid = ui::Root::create({.input_capacity = 1});
    assert(!invalid && invalid.error() == ui::EInitError::INVALID_INPUT_CAPACITY);
    assert(ImGui::GetCurrentContext() == original);
    auto first_font = (*first)->fontAtlas();
    auto second_font = (*second)->fontAtlas();
    assert(first_font && second_font && first_font->pixels == second_font->pixels);
    assert((*first)->feedInput(ui::Key{ui::EKey::A, true}, 100));
    assert((*second)->feedInput(ui::Key{ui::EKey::B, true}, 200));
    ui::DrawData slot;
    {
        auto draw_first = [] { ImGui::TextUnformatted("First root"); };
        auto& one = ui_test::makePane<TUiDrawPane<decltype(draw_first)>>(**first, draw_first);
        auto draw_second = [] { ImGui::TextUnformatted("Second root"); };
        auto& two = ui_test::makePane<TUiDrawPane<decltype(draw_second)>>(**second, draw_second);
        (*first)->deferChange(
            one,
            [](object::LuxObject& target) noexcept { static_cast<ui::Pane&>(target).setTitle("Adopted"); }
        );
        assert(one.title() == "Draw");
        assert((*first)->update({}));
        assert(one.title() == "Adopted");
        for (int i{}; i < 100; ++i)
        {
            assert((i % 2 ? *first : *second)->update({{640, 480}, 1.0F / 60.0F}, slot));
            assert(slot.valid() && ImGui::GetCurrentContext() == original);
        }
        const auto first_input = (*first)->inputSnapshot();
        const auto second_input = (*second)->inputSnapshot();
        assert(first_input.held[std::size_t(ui::EKey::A)] && !first_input.held[std::size_t(ui::EKey::B)]);
        assert(second_input.held[std::size_t(ui::EKey::B)] && !second_input.held[std::size_t(ui::EKey::A)]);
        assert(first_input.sequence == 100 && second_input.sequence == 200);
        auto refused = (*first)->update({{0, 480}, 0.016F}, slot);
        assert(!refused && refused.error() == ui::ECaptureError::INVALID_INPUT);
        assert(ImGui::GetCurrentContext() == original);
    }
    first->reset();
    second->reset();
    assert(slot.valid() && ImGui::GetCurrentContext() == original);
    assert(first_font->pixels == second_font->pixels); // The owning font copy outlives both backends.
    ImGui::DestroyContext(original);
}
