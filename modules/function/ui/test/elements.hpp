#pragma once
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <cassert>
#include <cmath>
#include <type_traits>

namespace element_checks
{
    using namespace lux;
    static_assert(!std::derived_from<ui::Pane, ui::Element>);
    static_assert(std::derived_from<ui::Layout, ui::Element>);

    class Item final : public ui::Element
    {
    public:
        template <class Parent>
        Item(Parent& parent, const char* id, ui::Size preferred = {40, 20})
            : ui::Element(parent, ui::ElementId{id}), preferred(preferred)
        {}
        ui::Size preferred;
        unsigned draws{}, updates{}, measures{}, events{};
        bool wrap{}, accept{};

    private:
        ui::SizeHint sizeHintContent() noexcept override
        {
            return {{10, 10}, preferred};
        }
        ui::SizeHint measureContent(float width) noexcept override
        {
            ++measures;
            const float height =
                wrap ? std::ceil(preferred.width / std::max(1.F, width)) * preferred.height : preferred.height;
            return {{10, 10}, {preferred.width, height}};
        }
        void draw() noexcept override
        {
            ++draws;
        }
        void update() noexcept override
        {
            ++updates;
        }
        void event(object::EventView& event) noexcept override
        {
            ++events;
            if (accept)
                event.accept();
        }
    };
    class Owner final : public ui::Pane
    {
    public:
        explicit Owner(ui::Root& root)
            : ui::Pane(root.dispatcherRef(), ui::PaneId{"owner"}, ui::PaneTypeId{"test.owner"}, "Owner"),
              item(std::make_unique<Item>(*this, "old"))
        {
            setContent(*item);
            ui_test::mount(root, *this);
        }
        std::unique_ptr<Item> item;
        bool replace{};

    private:
        void update() noexcept override
        {
            if (!std::exchange(replace, false))
                return;
            item = std::make_unique<Item>(*this, "new");
            setContent(*item);
        }
    };

    inline void run(object::ObjectDispatcherRef dispatcher)
    {
        auto created = ui::Root::create(dispatcher, {.docking = false});
        assert(created);
        auto& root = **created;
        {
            Owner owner(root);
            assert(root.requestFocus(*owner.item));
            assert(root.capturePointer(*owner.item));
            owner.replace = true;
            assert(root.update({}, nullptr));
            assert(!root.focusedElement() && owner.item->updates == 0);
            assert(root.update({}, nullptr));
            assert(owner.item->updates == 1);
            owner.setVisible(false);
            ui::Pane child(owner, ui::PaneId{"independent"}, ui::PaneTypeId{"test"}, "Independent");
            Item content(child, "content");
            child.setContent(content);
            assert(content.displayed() && !owner.item->displayed());
            assert(root.requestFocus(content));
            ui::DrawData draw;
            assert(root.update({{640, 480}, 0.016F}, &draw));
            assert(content.draws == 1 && owner.item->draws == 0);
        }
        ui::Pane pane(root.dispatcherRef(), ui::PaneId{"layout"}, ui::PaneTypeId{"test"}, "Layout");
        ui_test::mount(root, pane);
        {
            std::vector<std::unique_ptr<ui::Layout>> levels;
            levels.push_back(std::make_unique<ui::Layout>(pane, ui::ElementId{"deep"}));
            for (unsigned i = 1; i < 32; ++i)
                levels.push_back(std::make_unique<ui::Layout>(*levels.back(), ui::ElementId{"nested"}));
            {
                Item wrapped(*levels.back(), "leaf", {180, 20});
                wrapped.wrap = true;
                levels.front()->arrange({{}, {60, 200}});
                assert(wrapped.measures == 1 && wrapped.rect().size.width == 60);
                wrapped.measures = 0;
                assert(levels.front()->measure(90).preferred.height == 40);
                assert(wrapped.measures == 1);
            }
            while (!levels.empty())
                levels.pop_back();
        }
        {
            ui::Layout row(pane, ui::ElementId{"saturation"}, ui::ELayoutType::HORIZONTAL);
            row.setSpacing({0, 0});
            Item a(row, "a", {10, 20}), b(row, "b", {10, 20}), c(row, "c", {10, 20});
            a.setMaximumSize({20, 20});
            b.setMaximumSize({40, 20});
            c.setStretch({2, 1});
            row.arrange({{}, {200, 20}});
            assert(a.rect().size.width == 20 && b.rect().size.width == 40 && c.rect().size.width == 140);
            assert(c.rect().position.x == 60);
        }
        {
            ui::Layout layout(pane, ui::ElementId{"row"}, ui::ELayoutType::HORIZONTAL);
            layout.setSpacing({0, 0});
            pane.setContent(layout);
            Item one(layout, "one"), two(layout, "two");
            one.setMaximumSize({60, 50});
            two.setStretch({3, 1});
            layout.arrange({{}, {200, 40}});
            assert(one.rect().size.width == 60 && two.rect().size.width == 140);
            assert(two.rect().position.x == 60);
            layout.arrange({{}, {30, 30}});
            assert(one.rect().size.width == 15 && two.rect().size.width == 15);
            layout.arrange({{}, {4, 4}});
            assert(one.rect().size.width == 10 && two.rect().size.width == 10);
            two.setVisible(false);
            layout.arrange({{}, {80, 40}});
            assert(one.rect().size.width == 60);
            two.setVisible(true);
            two.setEnabled(false);
            layout.arrange({{}, {80, 40}});
            assert(two.rect().size.width == 40);
        }
        {
            ui::Layout form(pane, ui::ElementId{"form"}, ui::ELayoutType::FORM);
            form.setSpacing({4, 2});
            Item label(form, "label", {30, 20}), field(form, "field", {80, 20});
            Item second_label(form, "label2", {50, 20}), second_field(form, "field2", {80, 20});
            static_cast<void>(form.measure(200));
            form.arrange({{}, {200, 100}});
            assert(form.status() == ui::ELayoutStatus::VALID);
            assert(field.rect().position.x == 54 && second_field.rect().position.x == 54);
            assert(field.rect().size.width == 146);
            label.setVisible(false);
            form.arrange({{}, {200, 100}});
            assert(second_label.rect().position.y == 0 && second_field.rect().position.y == 0);
            Item unmatched(form, "unmatched");
            assert(form.status() == ui::ELayoutStatus::INCOMPLETE_FORM);
            Item partner(form, "partner");
            assert(form.status() == ui::ELayoutStatus::VALID); // No draw/measure needed after a structure change.
        }
        {
            ui::Layout grid(pane, ui::ElementId{"grid"}, ui::ELayoutType::GRID);
            grid.setColumns(2);
            grid.setSpacing({4, 6});
            grid.setMargins({2, 3, 2, 3});
            Item a(grid, "a"), b(grid, "b"), c(grid, "c");
            grid.arrange({{}, {108, 62}});
            assert(a.rect().position.x == 2 && b.rect().position.x == 56);
            assert(c.rect().position.y == 34);
            b.setVisible(false);
            grid.arrange({{}, {108, 62}});
            assert(c.rect().position.x == 56 && c.rect().position.y == 3);
        }
        {
            ui::Layout vertical(pane, ui::ElementId{"vertical"});
            vertical.setSpacing({0, 0});
            ui::Layout row(vertical, ui::ElementId{"nested"}, ui::ELayoutType::HORIZONTAL);
            Item text(row, "wrapped", {180, 20});
            text.wrap = true;
            const auto hint = vertical.measure(60);
            assert(hint.preferred.height == 60);
            pane.setContent(vertical);
            ui::DrawData draw;
            assert(root.update({{640, 480}, 0.016F}, &draw));
            assert(text.draws == 1 && text.updates == 1);
            assert(root.update({}, nullptr));
            assert(text.updates == 2);
        }
        {
            ui::Layout layout(pane, ui::ElementId{"controls"});
            pane.setContent(layout);
            ui::Button button(layout, ui::ElementId{"button"}, "Apply");
            ui::CheckBox check(layout, ui::ElementId{"check"}, "Enabled");
            ui::TextEdit text(layout, ui::ElementId{"text"}, "before");
            ui::NumericEdit number(layout, ui::ElementId{"number"}, 1.0F);
            ui::Choice choice(layout, ui::ElementId{"choice"}, {{1, "One"}, {2, "Two"}}, 1);
            ui::Label label(layout, ui::ElementId{"label"}, "A label that can wrap when space is limited.");
            label.setWrap(true);
            unsigned clicks{}, checks{}, text_changes{}, cancelled{};
            const auto button_connection =
                lux::object::LuxObject::connect(std::addressof(button), &ui::Button::activated, [&]() noexcept {
                    ++clicks;
                }).value();
            const auto check_connection = lux::object::LuxObject::connect(
                                              std::addressof(check),
                                              &ui::CheckBox::edited,
                                              [&](const ui::EditResult& result) noexcept {
                                                  assert(result.began && result.changed && result.committed);
                                                  ++checks;
                                              }
            ).value();
            const auto text_connection = lux::object::LuxObject::connect(
                                             std::addressof(text),
                                             &ui::TextEdit::edited,
                                             [&](const ui::EditResult& result) noexcept {
                                                 text_changes += result.changed;
                                                 cancelled += result.cancelled;
                                             }
            ).value();
            check.setValue(true);
            text.setValue("start");
            number.setValue(2.0F);
            choice.setValue(2);
            assert(checks == 0 && text_changes == 0 && std::get<float>(number.value()) == 2.F);
            assert(!number.setSpec({.minimum = std::int32_t{0}}));
            assert(number.setSpec({.minimum = 0.0F, .maximum = 10.0F}));
            root.setDockLayout({.center = "layout"});
            ui::DrawData draw;
            const auto frame = [&] { assert(root.update({{640, 480}, 0.016F}, &draw)); };
            for (unsigned i{}; i != 3; ++i)
                frame();
            const auto click = [&](ui::Element& element) {
                const auto origin = element.contentOrigin();
                assert(root.feedInput(ui::PointerMove{{origin.x + 8, origin.y + 8}}));
                frame();
                assert(root.feedInput(ui::PointerButton{ui::EPointerButton::LEFT, true}));
                frame();
                assert(root.feedInput(ui::PointerButton{ui::EPointerButton::LEFT, false}));
                frame();
            };
            click(button);
            assert(clicks == 1);
            click(check);
            assert(checks == 1 && !check.value());
            click(text);
            assert(root.feedInput(ui::Text{U'x'}));
            frame();
            assert(text_changes > 0 && text.value() != "start");
            const auto modified = text.value();
            ui::Command undo{ui::CommandIdView{"lux.edit.undo"}};
            assert(object::sendEvent(text, undo) && undo.enabled);
            undo.phase = ui::ECommandPhase::EXECUTE;
            assert(object::sendEvent(text, undo));
            frame();
            assert(text.value() == "start");
            ui::Command redo{ui::CommandIdView{"lux.edit.redo"}};
            assert(object::sendEvent(text, redo) && redo.enabled);
            redo.phase = ui::ECommandPhase::EXECUTE;
            assert(object::sendEvent(text, redo));
            frame();
            assert(text.value() == modified);
            assert(root.feedInput(ui::Key{ui::EKey::ESCAPE, true}));
            frame();
            assert(cancelled == 1 && text.value() == "start");
            assert(root.feedInput(ui::Key{ui::EKey::ESCAPE, false}));
            frame();
        }
    }
}
