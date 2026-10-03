#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <imgui.h>

#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include "elements.hpp"
#include "input_checks.hpp"

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
        LifetimeElement(Parent& parent, Lifetime& lifetime, const char* id)
            : Element(parent, lux::ui::ElementId{id}), lifetime_(lifetime)
        {}
        ~LifetimeElement() override
        {
            assert(lifetime_.resource_alive);
            ++lifetime_.elements_destroyed;
        }

    private:
        void draw() noexcept override {}
        Lifetime& lifetime_;
    };

    class LifetimePane final : public lux::ui::Pane
    {
    public:
        LifetimePane(lux::ui::Pane& parent, Lifetime& lifetime)
            : Pane(parent, lux::ui::PaneId{"child"}, lux::ui::PaneTypeId{"test.lifetime"}, "Child"),
              content_(*this, lifetime, "child-content"), lifetime_(lifetime)
        {
            setContent(content_);
        }
        ~LifetimePane() override
        {
            ++lifetime_.panes_destroyed;
        }

    private:
        LifetimeElement content_;
        Lifetime& lifetime_;
    };

    class LifetimeOwner final : public lux::ui::Pane
    {
    public:
        LifetimeOwner(lux::ui::Root& root, Lifetime& lifetime)
            : Pane(root.dispatcherRef(), lux::ui::PaneId{"lifetime"}, lux::ui::PaneTypeId{"test.lifetime"}, "Lifetime"),
              resource_{lifetime}, content_(*this, lux::ui::ElementId{"layout"}), fixed_(content_, lifetime, "fixed"),
              child_(*this, lifetime)
        {
            setContent(content_);
            fields_.push_back(std::make_unique<LifetimeElement>(content_, lifetime, "first"));
            fields_.push_back(std::make_unique<LifetimeElement>(content_, lifetime, "second"));
            ui_test::mount(root, *this);
            assert(root.requestFocus(*fields_.front()));
            assert(root.capturePointer(*fields_.front()));
            root.deferChange(*fields_.front(), [](lux::object::LuxObject&) noexcept { std::abort(); });
            root.deferChange(child_, [](lux::object::LuxObject&) noexcept { std::abort(); });
        }

    private:
        Resource resource_;
        lux::ui::Layout content_;
        LifetimeElement fixed_;
        LifetimePane child_;
        std::vector<std::unique_ptr<lux::ui::Element>> fields_;
    };

    class ChangeElement final : public lux::ui::Element
    {
    public:
        explicit ChangeElement(lux::ui::Pane& parent) : Element(parent, lux::ui::ElementId{"changing"}) {}
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
                self.root().deferChange(self, apply);
            if (self.on_apply)
                self.on_apply(self);
        }

    private:
        void draw() noexcept override
        {
            ++draws;
            if (defer_draw)
                root().deferChange(*this, apply);
            if (on_draw)
                on_draw(*this);
        }
        void update() noexcept override
        {
            ++updates;
            if (defer_update)
                root().deferChange(*this, apply);
            if (on_update)
                on_update(*this);
        }
        void event(lux::object::EventView&) noexcept override
        {
            root().deferChange(*this, apply);
            if (on_event)
                on_event(*this);
        }
    };

    class ChangeOwner final : public lux::ui::Pane
    {
    public:
        explicit ChangeOwner(lux::ui::Root& root)
            : Pane(root.dispatcherRef(), lux::ui::PaneId{"change"}, lux::ui::PaneTypeId{"test.change"}, "Change")
        {
            replace();
            ui_test::mount(root, *this);
        }
        std::optional<ChangeElement> content;
        void replace() noexcept
        {
            content.emplace(*this); // Deliberate address reuse: old intents must not follow the pointer.
            setContent(*content);
        }
    };

    void lifetimeAndChanges(lux::object::ObjectDispatcherRef dispatcher)
    {
        using namespace lux;
        auto created = ui::Root::create(dispatcher, {.docking = false});
        assert(created);
        auto& root = **created;
        Lifetime lifetime;
        {
            std::unique_ptr<ui::Pane> owner = std::make_unique<LifetimeOwner>(root, lifetime);
            assert(root.firstChild() == owner.get());
        }
        assert(!lifetime.resource_alive && lifetime.resource_destroyed == 1);
        assert(!root.firstChild() && !root.findPane(ui::PaneIdView{"child"}));
        assert(!root.focusedElement() && !root.focusedPane());
        root.applyPendingChanges(); // All callbacks attached to destroyed members were removed.
        assert(root.update({}, nullptr));

        // A failed factory reclaims a complete but unpublished candidate using the same ordinary deleter.
        Lifetime rejected;
        const auto make_candidate = [&]() -> std::unique_ptr<ui::Pane> {
            auto candidate = std::make_unique<LifetimeOwner>(root, rejected);
            return {};
        };
        assert(!make_candidate() && !root.firstChild() && rejected.resource_destroyed == 1);
        root.applyPendingChanges();

        ChangeOwner owner(root);
        auto& content = *owner.content;
        content.defer_draw = content.defer_update = true;
        ui::DrawData draw;
        assert(root.update({{640, 480}, 0.016F}, &draw));
        assert(content.draws == 1 && content.updates == 1 && content.applications == 0);
        ui::Command command{ui::CommandIdView{"test.change"}};
        static_cast<void>(object::sendEvent(content, command));
        static_cast<void>(object::routeEvent(content, root, command));
        assert(content.applications == 0);
        root.applyPendingChanges();
        assert(content.applications == 1); // Draw, maintenance and events coalesce.
        content.defer_draw = content.defer_update = false;
        root.deferChange(content, ChangeElement::apply);
        root.deferChange(content, [](object::LuxObject& target) noexcept {
            ++static_cast<ChangeElement&>(target).extra;
        });
        root.applyPendingChanges();
        assert(content.applications == 2 && content.extra == 1);
        content.repeat = true;
        root.deferChange(content, ChangeElement::apply);
        for (unsigned index{}; index < 1000; ++index)
        {
            root.applyPendingChanges();
            assert(content.applications == index + 3); // Never consumes the self-enqueued next batch.
        }
        content.repeat = false;
        root.applyPendingChanges();

        // A preceding owner callback destroys a later target, in both current and next batches.
        root.deferChange(owner, [](object::LuxObject& target) noexcept {
            auto& self = static_cast<ChangeOwner&>(target);
            self.root().deferChange(*self.content, ChangeElement::apply);
            self.replace();
            assert(!self.root().focusedElement());
        });
        root.deferChange(content, ChangeElement::apply);
        assert(root.requestFocus(content) && root.capturePointer(content));
        root.applyPendingChanges();
        root.applyPendingChanges();
        assert(owner.content->applications == 0 && owner.content->updates == 0);
        assert(root.update({}, nullptr) && owner.content->updates == 1);
        root.deferChange(*owner.content, ChangeElement::apply);
        owner.content.reset();
        assert(!owner.content && !owner.Pane::content());
        owner.replace();
        root.applyPendingChanges();
        assert(owner.content->applications == 0);
    }

    int contractViolation(std::string_view scenario, lux::object::ObjectDispatcherRef dispatcher)
    {
        using namespace lux;
        auto created = ui::Root::create(dispatcher, {.docking = false});
        assert(created);
        auto& root = **created;
        ChangeOwner owner(root);
        const auto apply = +[](ChangeElement& target) noexcept { target.root().applyPendingChanges(); };
        const auto destroy =
            +[](ChangeElement& target) noexcept { static_cast<ChangeOwner&>(target.pane()).content.reset(); };
        ui::Command command{ui::CommandIdView{"test.change"}};
        ui::DrawData draw;
        // Run these modes in a subprocess and require the production contract's abnormal exit.
        // Returning EXIT_FAILURE means the forbidden operation was allowed; it is not a passing probe.
        if (scenario == "apply-in-draw" || scenario == "destroy-in-draw")
        {
            owner.content->on_draw = scenario == "apply-in-draw" ? apply : destroy;
            static_cast<void>(root.update({{640, 480}, 0.016F}, &draw));
        }
        else if (scenario == "apply-in-event" || scenario == "destroy-in-event")
        {
            owner.content->on_event = scenario == "apply-in-event" ? apply : destroy;
            static_cast<void>(object::sendEvent(*owner.content, command));
        }
        else if (scenario == "apply-in-update")
        {
            owner.content->on_update = apply;
            static_cast<void>(root.update({}, nullptr));
        }
        else if (scenario == "recursive-apply" || scenario == "destroy-in-apply")
        {
            owner.content->on_apply = scenario == "recursive-apply" ? apply : destroy;
            root.deferChange(*owner.content, ChangeElement::apply);
            root.applyPendingChanges();
        }
        else if (scenario == "apply-in-signal")
        {
            auto visibility = object::LuxObject::connect(
                &owner,
                &ui::Pane::visibilityChanged,
                &owner,
                [&root](const ui::PaneVisibilityChanged&) noexcept { root.applyPendingChanges(); }
            );
            assert(visibility);
            owner.setVisible(false);
        }
        return EXIT_FAILURE;
    }

    class Probe final : public lux::ui::Pane
    {
    public:
        template <class Parent>
        Probe(Parent& parent, const char* id)
            : lux::ui::Pane(ui_test::parent(parent), lux::ui::PaneId{id}, lux::ui::PaneTypeId{"test.probe"}, id)
        {
            setContent(probe_content_);
            ui_test::mount(parent, *this);
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
                ImGui::GetIO().ConfigInputTrickleEventQueue = false;
            ImGui::TextUnformatted("CPU pane");
            if (edit_text)
            {
                ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Text", text.data(), text.size());
            }
            if (reject_capture)
            {
                const auto result = root().update({{400, 300}, 0.016F}, &reentrant);
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
                    assert(root().capturePointer(*transfer_capture));
                if (consume_keys)
                    event.accept();
            }
            if (input && std::holds_alternative<lux::ui::PointerMove>(*input))
            {
                ++moves;
                event.accept();
            }
            if (input)
                if (const auto* focus = std::get_if<lux::ui::WindowFocus>(input); focus && !focus->focused)
                    ++losses;
            if (const auto* command = event.getIf<lux::ui::Command>())
            {
                if (command->id == lux::ui::CommandIdView{"lux.edit.undo"})
                    ++undo;
                if (command->id == lux::ui::CommandIdView{"lux.edit.redo"})
                    ++redo;
                event.accept();
            }
        }
    };
}

int main(int argc, char** argv)
{
    using namespace lux;
    auto* original = ImGui::CreateContext();
    auto messages_created = object::ObjectMessageQueue::create(64);
    assert(messages_created);
    auto messages = std::move(*messages_created);
    if (argc == 2)
        return contractViolation(argv[1], messages.dispatcherRef());
    lifetimeAndChanges(messages.dispatcherRef());
    element_checks::run(messages.dispatcherRef());
    input_checks::run(messages.dispatcherRef());
    auto first = ui::Root::create(messages.dispatcherRef(), {.docking = false});
    auto second = ui::Root::create(messages.dispatcherRef(), {.docking = false});
    assert(first && second && ImGui::GetCurrentContext() == original);
    auto font = (*first)->fontAtlas();
    assert(font && font->pixels.size() == std::size_t(font->width) * font->height * 4);
    ui::DrawData slot;
    {
        Probe parent(**first, "parent");
        parent.nested = true;
        parent.reject_capture = true;
        Probe child(parent, "child");
        static_assert(!std::is_constructible_v<ui::Pane, object::LuxObject&, ui::PaneId, ui::PaneTypeId, std::string>);
        static_assert(!std::is_constructible_v<ui::Pane, ui::Root&, ui::PaneId, ui::PaneTypeId, std::string>);
        Probe sibling(**first, "sibling");
        Probe separate(**second, "separate");
        assert(&child.root() == first->get());
        assert((*first)->findPane(ui::PaneIdView{"child"}) == &child);
        assert((*first)->update({{640, 480}, 0.016F}, &slot));
        assert(parent.draws == 1 && child.draws == 1 && sibling.draws == 1 && separate.draws == 0);
        assert(ImGui::GetCurrentContext() == original && slot.valid());
        child.setVisible(false);
        assert((*first)->update({}, nullptr));
        assert(parent.updates == 2 && child.updates == 2 && sibling.updates == 2 && separate.updates == 0);
        assert((*first)->update({{640, 480}, 0.016F}, &slot));
        assert(parent.draws == 2 && child.draws == 1 && sibling.draws == 2);
        const auto invalid = (*first)->update({{640, 480}, NAN}, &slot);
        assert(!invalid && invalid.error() == ui::ECaptureError::INVALID_INPUT && slot.valid());
        assert(parent.draws == 2);
        child.setVisible(true);
        assert((*first)->requestFocus(child));
        assert((*first)->update({{640, 480}, 0.016F}, &slot));
        assert((*first)->focusedPane() == &child);
        assert((*first)->feedInput(ui::Key{ui::EKey::A, true}));
        assert((*first)->update({{640, 480}, 0.016F}, &slot));
        const auto before_hidden = child.keys;
        parent.setVisible(false);
        assert((*first)->requestFocus(child) && (*first)->capturePointer(child));
        assert((*first)->update({}, nullptr));
        assert(child.keys == before_hidden); // Same input is never routed twice.
        parent.setVisible(true);
        child.setVisible(false);
        assert(!(*first)->requestFocus(separate));
        assert(!(*first)->requestFocus(child));
        assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert(separate.draws == 1 && ImGui::GetCurrentContext() == original);
        auto temporary = std::make_unique<Probe>(**first, "temporary");
        assert((*first)->requestFocus(*temporary));
        assert((*first)->capturePointer(*temporary));
        temporary.reset();
        assert((*first)->update({{640, 480}, 0.016F}, &slot));
        assert(!(*first)->findPane(ui::PaneIdView{"temporary"}));
    }
    {
        Probe parent(**second, "images");
        parent.nested = true;
        parent.immediate_input = true;
        ui::Layout layout(parent, ui::ElementId{"content"});
        parent.setContent(layout);
        ui::ImageElement one(layout, ui::ElementId{"one"});
        ui::ImageElement two(layout, ui::ElementId{"two"});
        one.setStretch({0, 0});
        two.setStretch({0, 0});
        const render::RTextureHandle shared{0, 0};
        one.setImage(shared);
        two.setImage(shared);
        one.setSize({32, 24});
        two.setSize({32, 24});
        for (unsigned frame{}; frame != 3; ++frame)
            assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert(one.image() == shared && two.image() == shared);
        assert(one.displayedSize() == (ui::Size{32, 24}));
        assert(!one.interaction().resized);
        one.setSize({40, 28});
        assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert(one.interaction().resized && one.displayedSize() == (ui::Size{40, 28}));
        assert((*second)->requestFocus(one));
        const auto origin = one.contentOrigin();
        assert((*second)->feedInput(ui::PointerMove{{origin.x + 10, origin.y + 10}}));
        assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert((*second)->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, true}));
        assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert(one.interaction().hovered && one.interaction().right_clicked);
        assert(!two.interaction().right_clicked);
        assert(one.interaction().local_pointer == (ui::Point{10, 10}));
        assert((*second)->update({}, nullptr));
        assert((*second)->feedInput(ui::PointerButton{ui::EPointerButton::RIGHT, false}));
        assert((*second)->update({{640, 480}, 0.016F}, &slot));
        assert(!one.interaction().right_clicked && !one.interaction().resized);
        assert((*second)->update({}, nullptr));

        const auto has_image = [&] {
            return std::ranges::any_of(slot.textures(), [](auto texture) {
                return texture == render::RTextureHandle{0, 0};
            });
        };
        assert(has_image()); // Slot zero/generation zero must not become the font token.
        one.setImage({});
        assert((*second)->update({{640, 480}, 0.016F}, &slot) && has_image());
        two.setImage({});
        assert((*second)->update({{640, 480}, 0.016F}, &slot) && !has_image());
    }
    first->reset();
    second->reset();
    {
        auto made = ui::Root::create(messages.dispatcherRef(), {.docking = false});
        assert(made);
        auto& root = **made;
        Probe first(root, "capture-first"), second(root, "capture-second");
        first.immediate_input = true;
        for (unsigned frame{}; frame != 3; ++frame)
        {
            assert(root.update({{640, 480}, 0.016F}, &slot));
        }
        assert(root.requestFocus(first));
        assert(root.feedInput(ui::PointerMove{{-100, -100}}));
        assert(root.feedInput(ui::PointerButton{ui::EPointerButton::LEFT, true}));
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(root.capturePointer(first));
        assert(root.requestFocus(first));
        first.transfer_capture = &second;
        assert(root.feedInput(ui::Key{ui::EKey::A, true}));
        assert(root.feedInput(ui::PointerMove{{-120, -100}}));
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(first.keys == 1 && first.moves == 0 && second.moves == 1);
        assert(root.feedInput(ui::WindowFocus{false}));
        assert(first.losses == 0 && second.losses == 0); // No delivery from a platform callback.
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(first.losses == 1 && second.losses == 1);
        assert(!root.capturePointer(second));
        assert(root.feedInput(ui::WindowFocus{true}));
        assert(root.feedInput(ui::PointerMove{{-140, -100}}));
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(second.moves == 1); // Loss ended capture even without a physical button-up.
    }
    {
        auto bounded = ui::Root::create(messages.dispatcherRef(), {.docking = false, .input_capacity = 4});
        assert(bounded);
        auto& root = **bounded;
        Probe pane(root, "input");
        pane.consume_keys = false;
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016F}, &slot));
        }
        assert(root.requestFocus(pane));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, false}));
        const auto full = root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, false});
        assert(!full && full.error() == ui::EInputError::FULL);
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(pane.undo == 1); // Input is consumed in the same Root update.
        assert(root.update({}, nullptr));
        assert(root.update({}, nullptr));
        assert(pane.undo == 1 && pane.redo == 0);
        // The trickled release is not routed early; a rejected release can be retried.
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, false}));
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(root.update({{640, 480}, 0.016F}, &slot));
        assert(pane.undo == 1);
        pane.edit_text = true;
        for (unsigned index{}; index != 3; ++index)
        {
            assert(root.update({{640, 480}, 0.016F}, &slot));
        }
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        assert(root.update({{640, 480}, 0.016F}, &slot));
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
        const auto invalid = ui::Root::create(messages.dispatcherRef(), {.scale = scale});
        assert(!invalid && invalid.error() == ui::EInitError::INVALID_SCALE);
        assert(ImGui::GetCurrentContext() == original);
    }
    {
        auto normal = ui::Root::create(messages.dispatcherRef(), {.scale = 1.f});
        auto enlarged = ui::Root::create(messages.dispatcherRef(), {.scale = 2.f});
        assert(normal && enlarged);
        auto first = (*normal)->fontAtlas();
        auto second = (*enlarged)->fontAtlas();
        assert(first && second && first->pixels != second->pixels);
        assert(second->pixels.size() > first->pixels.size());
        assert((*normal)->scale() == 1.f && (*enlarged)->scale() == 2.f);
        // Framebuffer scale remains independent of font preparation.
        assert((*enlarged)->update({{640, 480}, 0.016f, {2, 2}}, &slot));
        assert(ImGui::GetCurrentContext() == original);
    }
    ui::FontSource invalid_font;
    const auto rejected = ui::Root::create(messages.dispatcherRef(), {.font = &invalid_font});
    assert(!rejected && rejected.error() == ui::EInitError::INVALID_FONT_DATA);
    assert(ImGui::GetCurrentContext() == original);
    ImGui::DestroyContext(original);
    std::cout << "PASS Root tree drawing, independent maintenance, RAII, capture and context isolation\n";
}
