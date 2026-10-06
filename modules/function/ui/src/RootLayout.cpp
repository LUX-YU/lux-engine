#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    SizeHint Root::measureElement(Element& element, float width, bool intrinsic) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->layout_state.depth++ == 0 && !impl_->drawing)
        {
            ++impl_->layout_state.epoch;
        }
        const auto epoch = impl_->layout_state.epoch;
        if (intrinsic)
        {
            if (element.hint_epoch_ != epoch)
            {
                element.intrinsic_hint_ = element.constrain(element.sizeHintContent());
                element.hint_epoch_ = epoch;
            }
        }
        else if (element.measure_epoch_ != epoch || element.measured_width_ != width)
        {
            element.measured_hint_ = element.constrain(element.measureContent(width));
            element.measure_epoch_ = epoch;
            element.measured_width_ = width;
        }
        --impl_->layout_state.depth;
        return intrinsic ? element.intrinsic_hint_ : element.measured_hint_;
    }
    void Root::arrangeElement(Element& element) noexcept
    {
        requireOwner();
        detail::ContextActivation context{impl_->context->native()};
        if (impl_->layout_state.depth++ == 0 && !impl_->drawing)
        {
            ++impl_->layout_state.epoch;
        }
        static_cast<void>(element.measure(element.rect().size.width));
        element.arrangeContent();
        --impl_->layout_state.depth;
    }
    void Root::drawElement(Element& element, Point parent_origin) noexcept
    {
        if (!impl_->drawing || element.attachedRoot() != this)
        {
            detail::failContract();
        }
        if (!element.visible_)
        {
            return;
        }
        auto& rect = element.rect_;
        element.draw_origin_ = {parent_origin.x + rect.position.x, parent_origin.y + rect.position.y};
        const auto origin = element.draw_origin_;
        const auto identity = element.objectId();
        ImGui::PushID(static_cast<int>(identity.index));
        ImGui::PushID(static_cast<int>(identity.gen));
        ImGui::SetCursorScreenPos({origin.x, origin.y});
        ImGui::PushClipRect({origin.x, origin.y}, {origin.x + rect.size.width, origin.y + rect.size.height}, true);
        ImGui::BeginDisabled(!element.enabled());
        ImGui::BeginGroup();
        auto* prior_focus = impl_->focus_state.draw_focused_element;
        auto* prior_hover = impl_->focus_state.draw_hovered_element;
        const bool requested = impl_->focus_state.pending_element == &element;
        if (requested)
        {
            ImGui::SetKeyboardFocusHere();
        }
        element.draw();
        ImGui::EndGroup();
        element.hovered_ = ImGui::IsItemHovered();
        const bool clicked =
            element.hovered_ && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2));
        const bool retained = focusedElement() == &element && ImGui::IsWindowFocused();
        if (impl_->focus_state.draw_hovered_element == prior_hover && element.hovered_)
        {
            impl_->focus_state.draw_hovered_element = &element;
        }
        if (impl_->focus_state.draw_focused_element == prior_focus && element.enabled() &&
            (requested || clicked || retained || ImGui::IsItemFocused()))
        {
            impl_->focus_state.draw_focused_element = &element;
        }
        if (requested)
        {
            impl_->focus_state.pending_element = {};
        }
        ImGui::EndDisabled();
        ImGui::PopClipRect();
        ImGui::PopID();
        ImGui::PopID();
    }

    void Root::drawPane(Pane& pane) noexcept
    {
        if (!pane.visible())
        {
            if (pane.modal_)
            {
                for (int i = 0; i < impl_->context->native()->OpenPopupStack.Size; ++i)
                {
                    if (auto* window = impl_->context->native()->OpenPopupStack[i].Window;
                        window && std::strcmp(window->Name, pane.imgui_label_.c_str()) == 0)
                    {
                        ImGui::ClosePopupToLevel(i, true);
                        break;
                    }
                }
            }
            return;
        }
        // Give a new window a usable first-frame content region. Saved and docked
        // geometry still wins; layout never needs last frame's measured height.
        if (!ImGui::FindWindowByName(pane.imgui_label_.c_str()))
        {
            const auto display = ImGui::GetIO().DisplaySize;
            Size preferred{480, 320};
            if (pane.content_)
            {
                preferred = pane.content_->measure(std::max(0.F, display.x)).preferred;
            }
            const auto padding = ImGui::GetStyle().WindowPadding;
            ImGui::SetNextWindowSize(
                {std::min(display.x, std::max(240.F, preferred.width + padding.x * 2)),
                 std::min(display.y, std::max(160.F, preferred.height + padding.y * 2 + ImGui::GetFrameHeight()))},
                ImGuiCond_FirstUseEver
            );
        }
        ImGuiWindowFlags flags{};
        if (impl_->focus_state.pending_focus == &pane)
        {
            ImGui::SetNextWindowFocus();
            impl_->focus_state.pending_focus = {};
        }
        bool visible = true;
        if (pane.modal_ && !ImGui::IsPopupOpen(pane.imgui_label_.c_str()))
        {
            ImGui::OpenPopup(pane.imgui_label_.c_str());
        }
        const bool shown =
            pane.modal_
                ? ImGui::BeginPopupModal(pane.imgui_label_.c_str(), &visible, flags | ImGuiWindowFlags_NoDocking)
                : ImGui::Begin(pane.imgui_label_.c_str(), &visible, flags);
        if (shown)
        {
            if (pane.modal_)
            {
                impl_->focus_state.modal = &pane;
            }
            // Record the Pane before drawing nested content.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                impl_->focus_state.draw_focused = &pane;
            }
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            {
                impl_->focus_state.draw_hovered = &pane;
            }
            drawPaneContent(pane);
        }
        if (pane.modal_)
        {
            if (shown)
            {
                ImGui::EndPopup();
            }
        }
        else
        {
            ImGui::End();
        }
        if (!visible)
        {
            pane.requestClose();
        }
    }

    void Root::drawPaneContent(Pane& pane) noexcept
    {
        if (!impl_->drawing)
        {
            detail::failContract();
        }
        if (!pane.content_)
        {
            return;
        }
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        const Size size{std::max(0.F, available.x), std::max(0.F, available.y)};
        pane.content_->arrange({{}, size});
        drawElement(*pane.content_, {origin.x, origin.y});
    }

} // namespace lux::ui
