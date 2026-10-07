#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "UiTestHelpers.hpp"
#include <imgui.h>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

#include "api_checks.hpp"
#include "elements.hpp"
#include "input_checks.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace
{
    struct Lifetime final
    {
        bool resource_alive{true};
        unsigned panes_destroyed{}, elements_destroyed{}, resource_destroyed{};
    };

    struct Resource final
    {
        Lifetime& lifetime;
        ~Resource()
        {
            assert(lifetime.elements_destroyed == 4 && lifetime.panes_destroyed == 1);
            lifetime.resource_alive = false;
            ++lifetime.resource_destroyed;
        }
    };

    class LifetimeElement final : public lux::ui::Element
    {
    public:
        template <class Parent>
        LifetimeElement(Parent& parent, Lifetime& lifetime, const char* id) : Element{}, lifetime_(lifetime)
        {
            assert(parent.addElement(*this));
        }
        ~LifetimeElement() override
        {
            assert(lifetime_.resource_alive);
            ++lifetime_.elements_destroyed;
        }

    private:
        void draw() noexcept override {}
        Lifetime& lifetime_;
    };

    class LifetimeOwner final : public lux::ui::Pane
    {
    public:
        LifetimeOwner(Lifetime& lifetime)
            : Pane("Lifetime"), resource_{lifetime}, content_{}, fixed_(content_, lifetime, "fixed"),
              child_(content_, lifetime, "child")
        {
            assert(addElement(content_));
            fields_.push_back(std::make_unique<LifetimeElement>(content_, lifetime, "first"));
            fields_.push_back(std::make_unique<LifetimeElement>(content_, lifetime, "second"));
        }
        ~LifetimeOwner() override
        {
            ++resource_.lifetime.panes_destroyed;
        }
        void capture()
        {
            assert(root().requestFocus(*fields_.front()));
            assert(root().capturePointer(*fields_.front()));
            root().deferChange(*fields_.front(), [](lux::object::LuxObject&) noexcept { std::abort(); });
            root().deferChange(child_, [](lux::object::LuxObject&) noexcept { std::abort(); });
        }

    private:
        Resource resource_;
        lux::ui::Layout content_;
        LifetimeElement fixed_;
        LifetimeElement child_;
        std::vector<std::unique_ptr<lux::ui::Element>> fields_;
    };

    class ChangeElement final : public lux::ui::Element
    {
    public:
        ChangeElement() : Element{} {}
        unsigned applications{}, updates{}, draws{}, extra{};
        bool defer_draw{}, defer_update{}, repeat{};
        void (*on_apply)(ChangeElement&) noexcept {};
        void (*on_draw)(ChangeElement&) noexcept {};
        void (*on_event)(ChangeElement&) noexcept {};
        void (*on_update)(ChangeElement&) noexcept {};

        static void apply(lux::object::LuxObject& object) noexcept
        {
            auto& self = static_cast<ChangeElement&>(object);
            ++self.applications;
            if (self.repeat)
            {
                self.root().deferChange(self, apply);
            }
            if (self.on_apply)
            {
                self.on_apply(self);
            }
        }

    private:
        void draw() noexcept override
        {
            ++draws;
            if (defer_draw)
            {
                root().deferChange(*this, apply);
            }
            if (on_draw)
            {
                on_draw(*this);
            }
        }
        void update() noexcept override
        {
            ++updates;
            if (defer_update)
            {
                root().deferChange(*this, apply);
            }
            if (on_update)
            {
                on_update(*this);
            }
        }
        void event(lux::object::EventView&) noexcept override
        {
            root().deferChange(*this, apply);
            if (on_event)
            {
                on_event(*this);
            }
        }
    };

    class ChangeOwner final : public lux::ui::Pane
    {
    public:
        ChangeOwner() : Pane("Change")
        {
            replace();
        }
        std::optional<ChangeElement> content;
        void replace() noexcept
        {
            content.emplace(); // Deliberate address reuse: old intents must not follow the pointer.
            assert(addElement(*content));
        }
    };

    void lifetimeAndChanges()
    {
        using namespace lux;
        auto created = ui::Root::create({.docking = false});
        assert(created);
        auto& root = **created;
        Lifetime lifetime;
        {
            auto& owner = ui_test::makePane<LifetimeOwner>(root, lifetime);
            owner.capture();
            assert(root.firstChild() == &owner);
            assert(root.clearPanes());
        }
        assert(!lifetime.resource_alive && lifetime.resource_destroyed == 1);
        assert(!root.firstChild() && (ui_test::paneCount(root) == 0));
        assert(!root.focusedElement() && !root.focusedPane());
        ui_test::apply(root); // All callbacks attached to destroyed members were removed.
        assert(root.update());

        // A failed factory reclaims a complete but unpublished candidate using the same ordinary deleter.
        Lifetime rejected;
        const auto make_candidate = [&]() -> std::unique_ptr<ui::Pane>
        {
            auto candidate = std::make_unique<LifetimeOwner>(rejected);
            return {};
        };
        assert(!make_candidate() && !root.firstChild() && rejected.resource_destroyed == 1);
        ui_test::apply(root);

        auto& owner = ui_test::makePane<ChangeOwner>(root);
        auto& content = *owner.content;
        content.defer_draw = content.defer_update = true;
        ui::DrawData draw;
        assert(root.update({{640, 480}, 0.016F}, draw));
        assert(content.draws == 1 && content.updates == 1 && content.applications == 0);
        ui::Command command{ui::CommandIdView{"test.change"}};
        static_cast<void>(object::sendEvent(content, command));
        static_cast<void>(object::routeEvent(content, root, command));
        assert(content.applications == 0);
        ui_test::apply(root);
        assert(content.applications == 1); // Draw, maintenance and events coalesce.
        content.defer_draw = content.defer_update = false;
        root.deferChange(content, ChangeElement::apply);
        root.deferChange(
            content,
            [](object::LuxObject& target) noexcept { ++static_cast<ChangeElement&>(target).extra; }
        );
        ui_test::apply(root);
        assert(content.applications == 2 && content.extra == 1);
        content.repeat = true;
        root.deferChange(content, ChangeElement::apply);
        for (unsigned index{}; index < 1000; ++index)
        {
            ui_test::apply(root);
            assert(content.applications == index + 3); // Never consumes the self-enqueued next batch.
        }
        content.repeat = false;
        ui_test::apply(root);

        // A preceding owner callback destroys a later target, in both current and next batches.
        root.deferChange(
            owner,
            [](object::LuxObject& target) noexcept
            {
                auto& self = static_cast<ChangeOwner&>(target);
                self.root().deferChange(*self.content, ChangeElement::apply);
                self.replace();
                assert(!self.root().focusedElement());
            }
        );
        root.deferChange(content, ChangeElement::apply);
        assert(root.requestFocus(content) && root.capturePointer(content));
        ui_test::apply(root);
        ui_test::apply(root);
        assert(owner.content->applications == 0 && owner.content->updates == 2);
        assert(root.update() && owner.content->updates == 3);
        root.deferChange(*owner.content, ChangeElement::apply);
        owner.content.reset();
        assert(!owner.content && !owner.Pane::content());
        owner.replace();
        ui_test::apply(root);
        assert(owner.content->applications == 0);
    }

    int contractViolation(std::string_view scenario)
    {
        std::cout << "UI contract probe entered\n" << std::flush;
        using namespace lux;
        auto created = ui::Root::create({.docking = false});
        assert(created);
        auto& root = **created;
        auto& owner = ui_test::makePane<ChangeOwner>(root);
        const auto apply = +[](ChangeElement& target) noexcept
        {
            const auto result = target.root().update();
            assert(!result && result.error() == ui::ECaptureError::FRAME_OPEN);
            ++target.extra;
        };
        if (scenario == "modal-wrong-thread")
        {
            ui::Pane detached("Detached");
            std::thread worker([&detached] { detached.setModal(true); });
            worker.join();
            return EXIT_FAILURE;
        }
        const auto destroy =
            +[](ChangeElement& target) noexcept { static_cast<ChangeOwner&>(target.pane()).content.reset(); };
        ui::Command command{ui::CommandIdView{"test.change"}};
        ui::DrawData draw;
        // Run these modes in a subprocess and require the production contract's abnormal exit.
        // Returning EXIT_FAILURE means the forbidden operation was allowed; it is not a passing probe.
        if (scenario == "apply-in-draw" || scenario == "destroy-in-draw")
        {
            owner.content->on_draw = scenario == "apply-in-draw" ? apply : destroy;
            static_cast<void>(root.update({{640, 480}, 0.016F}, draw));
        }
        else if (scenario == "apply-in-event" || scenario == "destroy-in-event")
        {
            owner.content->on_event = scenario == "apply-in-event" ? apply : destroy;
            static_cast<void>(object::sendEvent(*owner.content, command));
        }
        else if (scenario == "apply-in-update")
        {
            owner.content->on_update = apply;
            static_cast<void>(root.update());
        }
        else if (scenario == "recursive-apply" || scenario == "destroy-in-apply")
        {
            owner.content->on_apply = scenario == "recursive-apply" ? apply : destroy;
            root.deferChange(*owner.content, ChangeElement::apply);
            ui_test::apply(root);
        }
        else if (scenario == "apply-in-signal")
        {
            auto visibility = object::LuxObject::connect(
                &owner,
                &ui::Pane::visibilityChanged,
                &owner,
                [&owner, apply](const ui::PaneVisibilityChanged&) noexcept { apply(*owner.content); }
            );
            assert(visibility);
            owner.setVisible(false);
        }
        if (scenario.find("destroy") == std::string_view::npos)
        {
            assert(owner.content->extra == 1);
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }

    class Probe final : public lux::ui::Pane
    {
    public:
        explicit Probe(const char* id) : lux::ui::Pane(id)
        {
            assert(addElement(probe_content_));
        }
        unsigned draws{}, updates{}, keys{}, undo{}, redo{}, moves{}, losses{};
        bool nested{}, reject_capture{}, edit_text{}, consume_keys{true}, immediate_input{};
        Probe* transfer_capture{};
        std::array<char, 32> text{};
        lux::ui::DrawData reentrant;

    private:
    public:
        TUiTestContent<Probe> probe_content_{*this};
        void drawTestContent(lux::ui::Element&) noexcept
        {
            ++draws;
            if (immediate_input)
            {
                ImGui::GetIO().ConfigInputTrickleEventQueue = false;
            }
            ImGui::TextUnformatted("CPU pane");
            if (edit_text)
            {
                ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Text", text.data(), text.size());
            }
            if (reject_capture)
            {
                const auto result = root().update({{400, 300}, 0.016F}, reentrant);
                assert(!result && result.error() == lux::ui::ECaptureError::FRAME_OPEN);
            }
        }
        void update() noexcept override
        {
            ++updates;
        }
        void event(lux::object::EventView& event) noexcept override
        {
            const auto* input = event.getIf<lux::ui::VInputEvent>();
            if (input && std::holds_alternative<lux::ui::Key>(*input))
            {
                ++keys;
                if (transfer_capture)
                {
                    assert(root().capturePointer(*transfer_capture));
                }
                if (consume_keys)
                {
                    event.accept();
                }
            }
            if (input && std::holds_alternative<lux::ui::PointerMove>(*input))
            {
                ++moves;
                event.accept();
            }
            if (input)
            {
                if (const auto* focus = std::get_if<lux::ui::WindowFocus>(input); focus && !focus->focused)
                {
                    ++losses;
                }
            }
            if (auto* command = event.getIf<lux::ui::Command>())
            {
                command->enabled = true;
                if (command->phase == lux::ui::ECommandPhase::QUERY)
                {
                    event.accept();
                    return;
                }
                if (command->id == lux::ui::CommandIdView{"lux.edit.undo"})
                {
                    ++undo;
                }
                if (command->id == lux::ui::CommandIdView{"lux.edit.redo"})
                {
                    ++redo;
                }
                event.accept();
            }
        }
    };
} // namespace

int main(int argc, char** argv)
{
    using namespace lux;
    auto* original = ImGui::CreateContext();
    auto& messages_created = object::ObjectRuntime::instance();
    auto& messages = messages_created;
    if (argc == 2)
    {
        return contractViolation(argv[1]);
    }
    lifetimeAndChanges();
    api_checks::run();
    element_checks::run();
    input_checks::run();
    auto first = ui::Root::create({.docking = false});
    auto second = ui::Root::create({.docking = false});
    assert(first && second && ImGui::GetCurrentContext() == original);
    auto font = (*first)->fontAtlas();
    assert(font && font->pixels.size() == std::size_t(font->width) * font->height * 4);
    ui::DrawData slot;
    {
        auto& parent = ui_test::makePane<Probe>(**first, "parent");
        parent.nested = true;
        parent.reject_capture = true;
        auto& child = ui_test::makePane<Probe>(**first, "child");
        static_assert(!std::is_constructible_v<ui::Pane, object::LuxObject&, ui::PaneId, std::string>);
        static_assert(!std::is_constructible_v<ui::Pane, ui::Root&, ui::PaneId, std::string>);
        auto& sibling = ui_test::makePane<Probe>(**first, "sibling");
        auto& separate = ui_test::makePane<Probe>(**second, "separate");
        assert(&child.root() == first->get());
        assert(ui_test::resolvePane(**first, ui_test::paneHandle(child)) == &child);
        assert((*first)->update({{640, 480}, 0.016F}, slot));
        assert(parent.draws == 1 && child.draws == 1 && sibling.draws == 1 && separate.draws == 0);
        assert(ImGui::GetCurrentContext() == original && slot.valid());
        child.setVisible(false);
        assert((*first)->update());
        assert(parent.updates == 2 && child.updates == 2 && sibling.updates == 2 && separate.updates == 0);
        assert((*first)->update({{640, 480}, 0.016F}, slot));
        assert(parent.draws == 2 && child.draws == 1 && sibling.draws == 2);
        const auto invalid = (*first)->update({{640, 480}, NAN}, slot);
        assert(!invalid && invalid.error() == ui::ECaptureError::INVALID_INPUT && slot.valid());
        assert(parent.draws == 2);
        child.setVisible(true);
        assert((*first)->requestFocus(child));
        assert((*first)->update({{640, 480}, 0.016F}, slot));
        assert((*first)->focusedPane() == &child);
        assert((*first)->feedInput(ui::Key{ui::EKey::A, true}));
        assert((*first)->update({{640, 480}, 0.016F}, slot));
        const auto before_hidden = child.keys;
        parent.setVisible(false);
        assert((*first)->requestFocus(child) && (*first)->capturePointer(child));
        assert((*first)->update());
        assert(child.keys == before_hidden); // Same input is never routed twice.
        parent.setVisible(true);
        child.setVisible(false);
        assert(!(*first)->requestFocus(separate));
        assert(!(*first)->requestFocus(child));
        assert((*second)->update({{640, 480}, 0.016F}, slot));
        assert(separate.draws == 1 && ImGui::GetCurrentContext() == original);
        auto& temporary = ui_test::makePane<Probe>(**first, "temporary");
        const auto temporary_id = ui_test::paneHandle(temporary);
        assert((*first)->requestFocus(temporary));
        assert((*first)->capturePointer(temporary));
        assert((*first)->removePane(temporary));
        assert((*first)->update({{640, 480}, 0.016F}, slot));
        assert(!ui_test::resolvePane(**first, temporary_id));
    }
    assert((*first)->clearPanes() && (*second)->clearPanes());
    {
        auto& parent = ui_test::makePane<Probe>(**second, "images");
        parent.nested = true;
        parent.immediate_input = true;
        ui::Layout layout{};
        assert(parent.replaceContent(layout));
        ui::ImageElement one{};
        assert(layout.addElement(one));
        ui::ImageElement two{};
        assert(layout.addElement(two));
        one.setStretch({0, 0});
        two.setStretch({0, 0});
        const render::RTextureHandle shared{0, 0};
        one.setImage(shared);
        two.setImage(shared);
        one.setSize({32, 24});
        two.setSize({32, 24});
        for (unsigned frame{}; frame != 3; ++frame)
        {
            assert((*second)->update({{640, 480}, 0.016F}, slot));
        }
        assert(one.image() == shared && two.image() == shared);
        assert(one.displayedSize() == (ui::Size{32, 24}));
        assert(!one.interaction().resized);
        one.setSize({40, 28});
        assert((*second)->update({{640, 480}, 0.016F}, slot));
        assert(one.interaction().resized && one.displayedSize() == (ui::Size{40, 28}));
        assert((*second)->requestFocus(one));
        const auto origin = one.contentOrigin();
        assert((*second)->feedInput(ui::PointerMove{{origin.x + 10, origin.y + 10}}));
        assert((*second)->update({{640, 480}, 0.016F}, slot));
        assert((*second)->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, true}));
        assert((*second)->update({{640, 480}, 0.016F}, slot));
        assert(one.interaction().hovered && one.interaction().right_clicked);
        assert(!two.interaction().right_clicked);
        assert(one.interaction().local_pointer == (ui::Point{10, 10}));
        assert((*second)->update());
        assert((*second)->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, false}));
        assert((*second)->update({{640, 480}, 0.016F}, slot));
        assert(!one.interaction().right_clicked && !one.interaction().resized);
        assert((*second)->update());

        const auto has_image = [&]
        {
            return std::ranges::any_of(
                slot.textures(),
                [](auto texture) { return texture == render::RTextureHandle{0, 0}; }
            );
        };
        assert(has_image()); // Slot zero/generation zero must not become the font token.
        one.setImage({});
        assert((*second)->update({{640, 480}, 0.016F}, slot) && has_image());
        two.setImage({});
        assert((*second)->update({{640, 480}, 0.016F}, slot) && !has_image());
    }
    first->reset();
    second->reset();
    {
        auto made = ui::Root::create({.docking = false});
        assert(made);
        auto& root = **made;
        auto& first = ui_test::makePane<Probe>(root, "capture-first");
        auto& second = ui_test::makePane<Probe>(root, "capture-second");
        first.immediate_input = true;
        for (unsigned frame{}; frame != 3; ++frame)
        {
            assert(root.update({{640, 480}, 0.016F}, slot));
        }
        assert(root.requestFocus(first));
        assert(root.feedInput(ui::PointerMove{{-100, -100}}));
        assert(root.feedInput(ui::PointerButton{ui::EPointerButton::LEFT, true}));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(root.capturePointer(first));
        assert(root.requestFocus(first));
        first.transfer_capture = &second;
        assert(root.feedInput(ui::Key{ui::EKey::A, true}));
        assert(root.feedInput(ui::PointerMove{{-120, -100}}));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(first.keys == 1 && first.moves == 0 && second.moves == 1);
        assert(root.feedInput(ui::WindowFocus{false}));
        assert(first.losses == 0 && second.losses == 0); // No delivery from a platform callback.
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(first.losses == 1 && second.losses == 1);
        assert(!root.capturePointer(second));
        assert(root.feedInput(ui::WindowFocus{true}));
        assert(root.feedInput(ui::PointerMove{{-140, -100}}));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(second.moves == 1); // Loss ended capture even without a physical button-up.
    }
    {
        auto bounded = ui::Root::create({.docking = false, .input_capacity = 4});
        assert(bounded);
        auto& root = **bounded;
        auto& pane = ui_test::makePane<Probe>(root, "input");
        pane.consume_keys = false;
        root.setMenu(
            {{ui::CommandId{"lux.edit.undo"}, "Undo", "Ctrl+Z", {ui::EKey::Z, true}},
             {ui::CommandId{"lux.edit.redo"}, "Redo", "Ctrl+Y", {ui::EKey::Y, true}}}
        );
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016F}, slot));
        }
        assert(root.requestFocus(pane));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, false}));
        const auto full = root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, false});
        assert(!full && full.error() == ui::EInputError::FULL);
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(pane.undo == 0); // Shortcut input is consumed; command waits for the structural safe point.
        assert(root.update());
        assert(pane.undo == 1);
        assert(root.update());
        assert(root.update());
        assert(pane.undo == 1 && pane.redo == 0);
        // The trickled release is not routed early; a rejected release can be retried.
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, false}));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(pane.undo == 1);
        pane.edit_text = true;
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016F}, slot));
        }
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.update({{640, 480}, 0.016F}, slot));
        assert(pane.undo == 1); // Text editing owns Ctrl+Z; no document undo.
        const auto invalid = root.feedInput(ui::PointerMove{{NAN, 0}});
        assert(!invalid && invalid.error() == ui::EInputError::INVALID_INPUT);
        root.closeInput();
        const auto closed = root.feedInput(ui::WindowFocus{false});
        assert(!closed && closed.error() == ui::EInputError::CLOSED);
    }
    assert(slot.valid() && ImGui::GetCurrentContext() == original);
    for (const float scale : std::array{0.f, -1.f, 4.1f, INFINITY, NAN})
    {
        const auto invalid = ui::Root::create({.scale = scale});
        assert(!invalid && invalid.error() == ui::EInitError::INVALID_SCALE);
        assert(ImGui::GetCurrentContext() == original);
    }
    {
        auto normal = ui::Root::create({.scale = 1.f});
        auto enlarged = ui::Root::create({.scale = 2.f});
        assert(normal && enlarged);
        auto first = (*normal)->fontAtlas();
        auto second = (*enlarged)->fontAtlas();
        assert(first && second && first->pixels != second->pixels);
        assert(second->pixels.size() > first->pixels.size());
        assert((*normal)->scale() == 1.f && (*enlarged)->scale() == 2.f);
        // Framebuffer scale remains independent of font preparation.
        assert((*enlarged)->update({{640, 480}, 0.016f, {2, 2}}, slot));
        assert(ImGui::GetCurrentContext() == original);
    }
    ui::FontSource invalid_font;
    const auto rejected = ui::Root::create({.font = &invalid_font});
    assert(!rejected && rejected.error() == ui::EInitError::INVALID_FONT_DATA);
    assert(ImGui::GetCurrentContext() == original);
    ImGui::DestroyContext(original);
    std::cout << "PASS Root tree drawing, independent maintenance, RAII, capture and context isolation\n";
}
