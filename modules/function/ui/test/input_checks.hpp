#pragma once
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <cassert>
#include <exception>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

namespace input_checks
{
    using namespace lux;
    class Content final : public ui::Element
    {
    public:
        Content() : ui::Element{} {}
        unsigned keys{}, compositions{}, cancellations{}, focus_losses{};

    private:
        void draw() noexcept override {}
        void event(object::EventView& event) noexcept override
        {
            if (auto* input = event.getIf<ui::VInputEvent>())
            {
                keys += std::holds_alternative<ui::Key>(*input);
                compositions += std::holds_alternative<ui::Composition>(*input);
                cancellations += std::holds_alternative<ui::PointerCancel>(*input);
                if (const auto* focus = std::get_if<ui::WindowFocus>(input))
                {
                    focus_losses += !focus->focused;
                }
            }
        }
    };
    class Window final : public ui::Pane
    {
    public:
        explicit Window(const char* name) : ui::Pane(name)
        {
            assert(addElement(content));
        }
        Content content;
        unsigned keys{};

    private:
        void event(object::EventView& event) noexcept override
        {
            if (auto* input = event.getIf<ui::VInputEvent>())
            {
                keys += std::holds_alternative<ui::Key>(*input);
            }
        }
    };
    inline void run()
    {
        auto made = ui::Root::create({.docking = true});
        assert(made);
        auto& root = **made;
        auto& owner = ui_test::makePane<Window>(root, "owner");
        auto& modal = ui_test::makePane<Window>(root, "modal");
        modal.setModal(true);
        modal.setVisible(false);
        ui::DrawData data;
        const auto turn = [&] { assert(root.update({{640, 480}, 0.016F}, data)); };
        for (int i = 0; i < 3; ++i)
        {
            turn();
        }
        assert(root.requestFocus(owner.content));
        assert(root.capturePointer(owner.content));
        owner.content.setVisible(false);
        assert(root.update({}));
        assert(owner.content.cancellations == 1);
        owner.content.setVisible(true);
        turn();
        assert(root.feedInput(ui::Key{ui::EKey::A, true}, 20));
        assert(root.feedInput(ui::Key{ui::EKey::A, false}, 21));
        turn();
        assert(owner.content.keys == 1 && root.inputSnapshot().sequence == 20);
        turn();
        assert(owner.content.keys == 2 && root.inputSnapshot().sequence == 21);
        assert(!root.feedInput(ui::Key{ui::EKey::B, true}, 21));
        assert(root.feedInput(ui::Composition{ui::ECompositionStage::STARTED}, 22));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}, 23));
        assert(root.feedInput(ui::Composition{ui::ECompositionStage::COMMITTED}, 24));
        assert(root.feedInput(ui::Key{ui::EKey::Z, false}, 25));
        turn();
        assert(owner.content.keys == 2 && owner.content.compositions == 2);
        assert(!root.inputSnapshot().composing && root.inputSnapshot().sequence == 24);
        turn();
        assert(owner.content.keys == 3 && root.inputSnapshot().sequence == 25);
        assert(root.feedInput(ui::Composition{ui::ECompositionStage::STARTED}, 26));
        assert(root.feedInput(ui::WindowFocus{false}, 27));
        turn();
        assert(!root.inputSnapshot().composing && owner.content.compositions == 4);
        assert(root.feedInput(ui::WindowFocus{true}, 28));
        turn();
        modal.setVisible(true);
        turn();
        assert(root.requestFocus(modal.content));
        assert(!root.requestFocus(owner.content));
        assert(!root.capturePointer(owner.content));
        turn();
        const auto outside = owner.keys;
        assert(root.feedInput(ui::Key{ui::EKey::B, true}, 29));
        turn();
        assert(modal.content.keys == 1 && modal.keys == 1 && owner.keys == outside);
        modal.setVisible(false);
        turn();
        assert(root.requestFocus(owner.content));
        turn();
        assert(root.capturePointer(owner.content));
        const auto losses = owner.content.focus_losses;
        assert(root.feedInput(ui::WindowFocus{false}, 30));
        assert(root.update({}));
        assert(owner.content.focus_losses == losses + 1);
        turn();
        assert(owner.content.focus_losses == losses + 1); // The later ImGui batch does not repeat cancellation.
    }
} // namespace input_checks
