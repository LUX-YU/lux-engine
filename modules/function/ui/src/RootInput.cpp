#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    Pane* Root::focusedPane() const noexcept
    {
        requireOwner();
        return impl_->focus_state.focused;
    }

    bool Root::requestFocus(Pane& pane) noexcept
    {
        requireOwner();
        const bool can_focus = pane.attachedRoot() == this && pane.visible() && allowedByModal(pane);
        if (!can_focus)
        {
            return false;
        }
        impl_->focus_state.pending_focus = &pane;
        return true;
    }

    bool Root::capturePointer(Pane& pane) noexcept
    {
        requireOwner();
        const bool can_capture =
            pane.attachedRoot() == this && pane.visible() && impl_->context->windowFocused() && allowedByModal(pane);
        if (!can_capture)
        {
            return false;
        }
        impl_->focus_state.pointer_capture = &pane;
        return true;
    }

    void Root::releasePointer(Pane& pane) noexcept
    {
        requireOwner();
        if (impl_->focus_state.pointer_capture.pane == &pane)
        {
            impl_->focus_state.pointer_capture = {};
        }
    }

    Element* Root::focusedElement() const noexcept
    {
        requireOwner();
        return impl_->focus_state.focused_element;
    }

    bool Root::requestFocus(Element& element) noexcept
    {
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() || !allowedByModal(element))
        {
            return false;
        }
        impl_->focus_state.pending_element = &element;
        return requestFocus(element.pane());
    }

    bool Root::capturePointer(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this || !element.displayed() || !element.enabled() ||
            !impl_->context->windowFocused() || !allowedByModal(element))
        {
            return false;
        }
        impl_->focus_state.pointer_capture = &element;
        return true;
    }

    void Root::releaseFocus(Element& element) noexcept
    {
        requireOwner();
        if (element.attachedRoot() != this)
        {
            detail::failContract();
        }
        if (impl_->focus_state.pending_element == &element)
        {
            impl_->focus_state.pending_element = {};
        }
        if (impl_->focus_state.focused_element != &element)
        {
            return;
        }
        detail::ContextActivation context{impl_->context->native()};
        ImGui::ClearActiveID();
        impl_->focus_state.focused_element = {};
        releasePointer(element);
    }

    void Root::releasePointer(Element& element) noexcept
    {
        requireOwner();
        if (impl_->focus_state.pointer_capture.element == &element)
        {
            impl_->focus_state.pointer_capture = {};
        }
    }

    Pane* Root::modalPane() const noexcept
    {
        detail::ContextActivation context{impl_->context->native()};
        const auto* modal = ImGui::GetTopMostPopupModal();
        if (!modal)
        {
            return nullptr;
        }
        auto* pane = impl_->focus_state.modal;
        return pane && pane->visible() && pane->modal_ && std::strcmp(pane->imgui_label_.c_str(), modal->Name) == 0
                   ? pane
                   : nullptr;
    }

    bool Root::allowedByModal(const Pane& target) const noexcept
    {
        detail::ContextActivation context{impl_->context->native()};
        return !ImGui::GetTopMostPopupModal() || modalPane() == &target;
    }

    bool Root::allowedByModal(const Element& target) const noexcept
    {
        return allowedByModal(target.pane());
    }

    void Root::deliverWindowFocus(bool focused_value) noexcept
    {
        const auto capture = impl_->focus_state.pointer_capture;
        impl_->context->setWindowFocused(focused_value);
        VInputEvent focus = WindowFocus{focused_value};
        Impl::Target focused{impl_->focus_state.focused_element};
        if (!focused)
        {
            focused = focusedPane();
        }
        if (!focused_value)
        {
            impl_->input_state.modifiers = 0;
            if (impl_->input_state.composing)
            {
                impl_->input_state.composing = false;
                VInputEvent cancelled = Composition{ECompositionStage::CANCELLED};
                if (focused)
                {
                    static_cast<void>(object::sendEvent(*focused, cancelled));
                }
            }
            impl_->focus_state.pointer_capture = {};
        }
        // Focus loss is an interaction-ending fact, not a command that
        // an ancestor or a modal can swallow on behalf of its owner.
        if (capture)
        {
            static_cast<void>(object::sendEvent(*capture, focus));
        }
        if (focused && focused != capture)
        {
            static_cast<void>(object::sendEvent(*focused, focus));
        }
    }

    void Root::routeInput() noexcept
    {
        const auto& io = ImGui::GetIO();
        auto* modal = modalPane();
        object::LuxObject& boundary = modal ? static_cast<object::LuxObject&>(*modal) : *this;
        // The association table stores only native identity/IME facts. ImGui's
        // queue remains the source of actual key, pointer and character events.
        const auto route = [&](const ImGuiInputEvent& input) noexcept
        {
            // An earlier handler can acquire, transfer or release capture. Resolve
            // the current owner for each event, never cache it for the whole batch.
            auto capture = impl_->focus_state.pointer_capture;
            if (input.Type == ImGuiInputEventType_Focus)
            {
                deliverWindowFocus(input.AppFocused.Focused);
                if (!input.AppFocused.Focused)
                {
                    impl_->context->cancelAdoptedInput();
                }
                return;
            }
            if (capture && (!capture.visible() || !allowedByModal(*capture.containingPane())))
            {
                impl_->focus_state.pointer_capture = {};
                VInputEvent cancelled = PointerCancel{};
                static_cast<void>(object::sendEvent(*capture, cancelled));
                capture = nullptr;
            }
            if (input.Type == ImGuiInputEventType_Key && (input.Key.Key & ImGuiMod_Mask_) != 0)
            {
                if (input.Key.Down)
                {
                    impl_->input_state.modifiers |= input.Key.Key;
                }
                else
                {
                    impl_->input_state.modifiers &= ~input.Key.Key;
                }
            }
            Impl::Target target{impl_->focus_state.focused_element};
            if (!target)
            {
                target = focusedPane();
            }
            std::optional<VInputEvent> value;
            switch (input.Type)
            {
            case ImGuiInputEventType_Key:
                if (impl_->input_state.composing || io.WantTextInput || ImGui::IsAnyItemActive() ||
                    !ImGui::TestKeyOwner(input.Key.Key, ImGuiKeyOwner_NoOwner))
                {
                    break;
                }
                if (const auto key = detail::Context::keyFromNative(input.Key.Key); key != EKey::NONE)
                {
                    value = Key{key, input.Key.Down};
                }
                break;
            case ImGuiInputEventType_MousePos:
                target = capture ? capture : Impl::Target{impl_->focus_state.hovered_element};
                if (!target)
                {
                    target = impl_->focus_state.hovered;
                }
                if (capture || !ImGui::IsAnyItemActive())
                {
                    value = PointerMove{{input.MousePos.PosX, input.MousePos.PosY}};
                }
                break;
            case ImGuiInputEventType_MouseButton:
                target = capture ? capture : Impl::Target{impl_->focus_state.hovered_element};
                if (!target)
                {
                    target = impl_->focus_state.hovered;
                }
                if (input.MouseButton.Button >= 0 && input.MouseButton.Button < 3 &&
                    (capture ||
                     ImGui::TestKeyOwner(ImGui::MouseButtonToKey(input.MouseButton.Button), ImGuiKeyOwner_NoOwner)))
                {
                    const auto button = input.MouseButton.Button == 0   ? EPointerButton::LEFT
                                        : input.MouseButton.Button == 1 ? EPointerButton::RIGHT
                                                                        : EPointerButton::MIDDLE;
                    value = PointerButton{button, input.MouseButton.Down};
                }
                break;
            case ImGuiInputEventType_MouseWheel:
                target = capture ? capture : Impl::Target{impl_->focus_state.hovered_element};
                if (!target)
                {
                    target = impl_->focus_state.hovered;
                }
                if (capture ||
                    (!ImGui::IsAnyItemActive() && ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, ImGuiKeyOwner_NoOwner)))
                {
                    value = PointerWheel{{input.MouseWheel.WheelX, input.MouseWheel.WheelY}};
                }
                break;
            default:
                break;
            }
            if (!value)
            {
                return;
            }
            if (target)
            {
                const bool blocked = !target.visible() || !allowedByModal(*target.containingPane());
                if (blocked || object::routeEvent(*target, boundary, *value))
                {
                    return;
                }
            }
            const auto* key = std::get_if<Key>(&*value);
            if (key && !modal && impl_->shortcut(*this, *key))
            {
                return;
            }
        };
        auto composition = [&](ECompositionStage stage) noexcept
        {
            impl_->input_state.composing = stage == ECompositionStage::STARTED || stage == ECompositionStage::UPDATED;
            Impl::Target target{impl_->focus_state.focused_element};
            if (!target)
            {
                target = focusedPane();
            }
            if (target && allowedByModal(*target.containingPane()))
            {
                VInputEvent event = Composition{stage};
                static_cast<void>(object::sendEvent(*target, event));
            }
        };
        impl_->context->consumeInput(route, composition);
        if (!ImGui::IsAnyMouseDown())
        {
            impl_->focus_state.pointer_capture = {};
        }
    }

    lux::cxx::expected<void, EInputError> Root::feedInput(const VInputEvent& event, std::uint64_t sequence) noexcept
    {
        requireOwner();
        return impl_->context->feedInput(event, sequence);
    }

    void Root::closeInput() noexcept
    {
        requireOwner();
        if (impl_->context->closeInput())
        {
            deliverWindowFocus(false);
        }
    }

    InputSnapshot Root::inputSnapshot() const noexcept
    {
        requireOwner();
        return impl_->context->inputSnapshot(impl_->input_state.composing, bool(impl_->focus_state.pointer_capture));
    }

} // namespace lux::ui
