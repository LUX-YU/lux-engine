#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/InputEvent.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <lux/engine/ui/Command.hpp>
namespace ImStb
{
#include <imstb_textedit.h>
}
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace lux::ui
{
    namespace
    {
        // ImGui's stb adapter uses these key codes; they are local to the UI backend.
        constexpr int textUndo = 0x20000A;
        constexpr int textRedo = 0x20000B;
        bool textCommand(object::EventView& event, bool editing, bool undo, bool redo, int& pending) noexcept
        {
            auto* command = event.getIf<Command>();
            if (!command || !editing)
                return false;
            const auto id = command->id.name();
            const bool is_undo = id == "lux.edit.undo";
            const bool is_redo = id == "lux.edit.redo";
            const bool other_text = id == "lux.edit.cut" || id == "lux.edit.copy" || id == "lux.edit.paste" ||
                                    id == "lux.edit.delete" || id == "lux.edit.select-all";
            if (!is_undo && !is_redo && !other_text)
                return false;
            command->enabled = (is_undo && undo) || (is_redo && redo);
            command->result = ECommandDispatchResult::DISABLED;
            if (command->phase == ECommandPhase::EXECUTE && command->enabled)
            {
                pending = is_undo ? textUndo : textRedo;
                command->result = ECommandDispatchResult::EXECUTED;
            }
            event.accept();
            return true;
        }
        ImGuiInputTextState* applyTextCommand(unsigned id, int& pending) noexcept
        {
            if (!pending)
                return nullptr;
            const auto command = std::exchange(pending, 0);
            if (auto* state = ImGui::GetInputTextState(id))
            {
                state->TextSrc = state->TextA.Data;
                state->OnKeyPressed(command);
                return state;
            }
            return nullptr;
        }
        void textUndoState(unsigned id, bool& undo, bool& redo) noexcept
        {
            const auto* state = ImGui::GetInputTextState(id);
            undo = state && state->Stb->undostate.undo_point > 0;
            redo = state && state->Stb->undostate.redo_point < IMSTB_TEXTEDIT_UNDOSTATECOUNT;
        }
        Size textSize(const std::string& text, float width = 0) noexcept
        {
            auto* font = ImGui::GetIO().Fonts->Fonts[0];
            const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, width, text.c_str());
            return {size.x, size.y};
        }
        SizeHint fieldHint(float width = 120.F) noexcept
        {
            const float height = ImGui::GetIO().Fonts->Fonts[0]->FontSize + ImGui::GetStyle().FramePadding.y * 2;
            return {{20, height}, {width, height}, {std::numeric_limits<float>::infinity(), height}};
        }
        EditResult editResult(bool changed) noexcept
        {
            return {changed, ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit(), false};
        }
        bool hasEdit(EditResult result) noexcept
        {
            return result.changed || result.began || result.committed || result.cancelled;
        }
        void requireOwner(const object::LuxObject& object) noexcept
        {
            if (!object.isOnAffinityThread())
                detail::failContract();
        }
    }
    Button::Button(ElementId id, std::string text)
        : Element(std::move(id)), text_(std::move(text)), label_(text_ + "###button")
    {
        setStretch({0, 0});
    }
    Button::Button(Pane& parent, ElementId id, std::string text)
        : Element(parent, std::move(id)), text_(std::move(text)), label_(text_ + "###button")
    {
        setStretch({0, 0});
    }
    Button::Button(Element& parent, ElementId id, std::string text)
        : Element(parent, std::move(id)), text_(std::move(text)), label_(text_ + "###button")
    {
        setStretch({0, 0});
    }
    void Button::setText(std::string text)
    {
        requireOwner(*this);
        text_ = std::move(text);
        label_ = text_ + "###button";
    }
    SizeHint Button::sizeHintContent() noexcept
    {
        auto size = textSize(text_);
        const auto padding = ImGui::GetStyle().FramePadding;
        size.width += padding.x * 2;
        size.height += padding.y * 2;
        return {size, size, size};
    }
    void Button::draw() noexcept
    {
        if (ImGui::Button(label_.c_str(), {rect().size.width, rect().size.height}))
            static_cast<void>(emit(activated));
    }
    Label::Label(ElementId id, std::string text) : Element(std::move(id)), text_(std::move(text))
    {
        setStretch({0, 0});
    }
    Label::Label(Pane& parent, ElementId id, std::string text) : Element(parent, std::move(id)), text_(std::move(text))
    {
        setStretch({0, 0});
    }
    Label::Label(Element& parent, ElementId id, std::string text)
        : Element(parent, std::move(id)), text_(std::move(text))
    {
        setStretch({0, 0});
    }
    void Label::setText(std::string text)
    {
        requireOwner(*this);
        text_ = std::move(text);
    }
    void Label::setWrap(bool wrap) noexcept
    {
        requireOwner(*this);
        wrap_ = wrap;
    }
    SizeHint Label::sizeHintContent() noexcept
    {
        const auto size = textSize(text_);
        return {{wrap_ ? 1.F : size.width, size.height}, size, {std::numeric_limits<float>::infinity(), size.height}};
    }
    SizeHint Label::measureContent(float width) noexcept
    {
        const auto size = textSize(text_, wrap_ ? std::max(1.F, width) : 0.F);
        return {{wrap_ ? 1.F : size.width, size.height}, size, {std::numeric_limits<float>::infinity(), size.height}};
    }
    void Label::draw() noexcept
    {
        if (wrap_)
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rect().size.width);
        ImGui::TextUnformatted(text_.c_str(), text_.c_str() + text_.size());
        if (wrap_)
            ImGui::PopTextWrapPos();
    }
    CheckBox::CheckBox(ElementId id, std::string text, bool value)
        : Element(std::move(id)), text_(std::move(text)), label_(text_ + "###value"), value_(value)
    {
        setStretch({0, 0});
    }
    CheckBox::CheckBox(Pane& parent, ElementId id, std::string text, bool value)
        : Element(parent, std::move(id)), text_(std::move(text)), label_(text_ + "###value"), value_(value)
    {
        setStretch({0, 0});
    }
    CheckBox::CheckBox(Element& parent, ElementId id, std::string text, bool value)
        : Element(parent, std::move(id)), text_(std::move(text)), label_(text_ + "###value"), value_(value)
    {
        setStretch({0, 0});
    }
    void CheckBox::setValue(bool value) noexcept
    {
        requireOwner(*this);
        value_ = value;
    }
    SizeHint CheckBox::sizeHintContent() noexcept
    {
        auto hint = fieldHint();
        hint.preferred.width = hint.preferred.height + ImGui::GetStyle().ItemInnerSpacing.x + textSize(text_).width;
        return hint;
    }
    void CheckBox::draw() noexcept
    {
        if (ImGui::Checkbox(label_.c_str(), &value_))
            static_cast<void>(emit(edited, EditResult{true, true, true, false}));
    }
    TextEdit::TextEdit(ElementId id, std::string value)
        : Element(std::move(id)), value_(std::move(value))
    {
        setStretch({1, 0});
    }
    TextEdit::TextEdit(Pane& parent, ElementId id, std::string value)
        : Element(parent, std::move(id)), value_(std::move(value))
    {
        setStretch({1, 0});
    }
    TextEdit::TextEdit(Element& parent, ElementId id, std::string value)
        : Element(parent, std::move(id)), value_(std::move(value))
    {
        setStretch({1, 0});
    }
    void TextEdit::setValue(std::string value)
    {
        requireOwner(*this);
        value_ = std::move(value);
        if (!editing_)
            before_ = value_;
    }
    void TextEdit::setHint(std::string hint)
    {
        requireOwner(*this);
        hint_ = std::move(hint);
    }
    void TextEdit::finishEdit(bool cancel) noexcept
    {
        requireOwner(*this);
        if (!std::exchange(editing_, false))
            return;
        if (cancel)
            value_ = before_;
        if (auto* attached = attachedRoot())
            attached->releaseFocus(*this);
        static_cast<void>(emit(edited, EditResult{cancel, false, !cancel, cancel}));
    }
    void TextEdit::event(object::EventView& event) noexcept
    {
        if (textCommand(event, editing_, can_undo_, can_redo_, text_command_))
            return;
        const auto* input = event.getIf<VInputEvent>();
        if (!input)
            return;
        const auto* focus = std::get_if<WindowFocus>(input);
        if ((focus && !focus->focused) || std::holds_alternative<PointerCancel>(*input))
            finishEdit();
    }
    SizeHint TextEdit::sizeHintContent() noexcept
    {
        return fieldHint();
    }
    void TextEdit::draw() noexcept
    {
        if (!editing_)
            before_ = value_;
        const bool cancel = editing_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (text_command_)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(rect().size.width);
        auto result = editResult(ImGui::InputTextWithHint("##value", hint_.c_str(), &value_));
        if (const auto* state = applyTextCommand(ImGui::GetItemID(), text_command_))
        {
            const std::string_view text{state->TextA.Data, static_cast<std::size_t>(state->TextLen)};
            result.changed = result.changed || value_ != text;
            value_.assign(text);
        }
        text_id_ = ImGui::GetItemID();
        textUndoState(text_id_, can_undo_, can_redo_);
        if (root().menuTargets(*this))
            result.committed = false;
        if (result.began)
            editing_ = true;
        if (cancel)
        {
            value_ = before_;
            result = {true, false, false, true};
        }
        const bool left_text = editing_ && !root().menuTargets(*this) && !ImGui::IsItemActive() && !result.cancelled;
        if (left_text)
            result.committed = true;
        if (result.committed || result.cancelled)
            editing_ = false;
        if (hasEdit(result))
            static_cast<void>(emit(edited, result));
    }
    NumericEdit::NumericEdit(ElementId id, VNumericValue value)
        : Element(std::move(id)), value_(value), before_(value)
    {
        setStretch({1, 0});
    }
    NumericEdit::NumericEdit(Pane& parent, ElementId id, VNumericValue value)
        : Element(parent, std::move(id)), value_(value), before_(value)
    {
        setStretch({1, 0});
    }
    NumericEdit::NumericEdit(Element& parent, ElementId id, VNumericValue value)
        : Element(parent, std::move(id)), value_(value), before_(value)
    {
        setStretch({1, 0});
    }
    void NumericEdit::setValue(VNumericValue value) noexcept
    {
        requireOwner(*this);
        if (value.index() != value_.index())
            detail::failContract();
        value_ = value;
    }
    void NumericEdit::finishEdit(bool cancel) noexcept
    {
        requireOwner(*this);
        if (!std::exchange(editing_, false))
            return;
        if (cancel)
            value_ = before_;
        if (auto* attached = attachedRoot())
            attached->releaseFocus(*this);
        static_cast<void>(emit(edited, EditResult{cancel, false, !cancel, cancel}));
    }
    void NumericEdit::event(object::EventView& event) noexcept
    {
        if (textCommand(event, editing_, can_undo_, can_redo_, text_command_))
            return;
        const auto* input = event.getIf<VInputEvent>();
        if (!input)
            return;
        const auto* focus = std::get_if<WindowFocus>(input);
        if ((focus && !focus->focused) || std::holds_alternative<PointerCancel>(*input))
            finishEdit();
    }
    bool NumericEdit::setSpec(NumericSpec spec) noexcept
    {
        requireOwner(*this);
        const auto same = [&](const auto& bound) { return !bound || bound->index() == value_.index(); };
        const bool valid =
            same(spec.minimum) && same(spec.maximum) && same(spec.step) && std::isfinite(spec.speed) && spec.speed > 0;
        if (!valid || (spec.mode == EScalarEditMode::SLIDER && (!spec.minimum || !spec.maximum)))
            return false;
        if (spec.minimum && spec.maximum && *spec.minimum > *spec.maximum)
            return false;
        spec_ = std::move(spec);
        return true;
    }
    SizeHint NumericEdit::sizeHintContent() noexcept
    {
        return fieldHint();
    }
    bool NumericEdit::valueValid() const noexcept
    {
        const bool finite = std::visit(
            [](auto value) {
                if constexpr (std::is_floating_point_v<decltype(value)>)
                    return std::isfinite(value);
                else
                    return true;
            },
            value_
        );
        return finite && (!spec_.minimum || value_ >= *spec_.minimum) && (!spec_.maximum || value_ <= *spec_.maximum);
    }
    void NumericEdit::draw() noexcept
    {
        const auto previous = value_;
        if (!editing_)
            before_ = value_;
        const bool cancel = editing_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (text_command_)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(rect().size.width);
        auto result = std::visit(
            [&](auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                constexpr auto data_type = [] {
                    if constexpr (std::same_as<Value, std::int32_t>)
                        return ImGuiDataType_S32;
                    else if constexpr (std::same_as<Value, std::uint32_t>)
                        return ImGuiDataType_U32;
                    else if constexpr (std::same_as<Value, std::int64_t>)
                        return ImGuiDataType_S64;
                    else if constexpr (std::same_as<Value, std::uint64_t>)
                        return ImGuiDataType_U64;
                    else if constexpr (std::same_as<Value, float>)
                        return ImGuiDataType_Float;
                    else
                        return ImGuiDataType_Double;
                }();
                const auto* minimum = spec_.minimum ? &std::get<Value>(*spec_.minimum) : nullptr;
                const auto* maximum = spec_.maximum ? &std::get<Value>(*spec_.maximum) : nullptr;
                const auto* step = spec_.step ? &std::get<Value>(*spec_.step) : nullptr;
                const auto* format = spec_.format.empty() ? nullptr : spec_.format.c_str();
                bool changed{};
                switch (spec_.mode)
                {
                case EScalarEditMode::INPUT:
                    changed = ImGui::InputScalar("##value", data_type, &value, step, nullptr, format);
                    break;
                case EScalarEditMode::DRAG:
                    changed = ImGui::DragScalar("##value", data_type, &value, spec_.speed, minimum, maximum, format);
                    break;
                case EScalarEditMode::SLIDER:
                    changed = ImGui::SliderScalar("##value", data_type, &value, minimum, maximum, format);
                    break;
                }
                if (const auto* state = applyTextCommand(ImGui::GetItemID(), text_command_))
                    changed |= ImGui::DataTypeApplyFromText(state->TextA.Data, data_type, &value, format);
                return editResult(changed);
            },
            value_
        );
        if (result.changed && !valueValid())
        {
            value_ = previous;
            result.changed = false;
        }
        text_id_ = ImGui::GetItemID();
        textUndoState(text_id_, can_undo_, can_redo_);
        if (root().menuTargets(*this))
            result.committed = false;
        if (result.began)
            editing_ = true;
        if (cancel)
        {
            value_ = before_;
            result = {true, false, false, true};
        }
        const bool left_text = editing_ && !root().menuTargets(*this) && !ImGui::IsItemActive() && !result.cancelled;
        if (left_text)
            result.committed = true;
        if (result.committed || result.cancelled)
            editing_ = false;
        if (hasEdit(result))
            static_cast<void>(emit(edited, result));
    }
    Choice::Choice(ElementId id, std::vector<ChoiceOption> options, std::int64_t value)
        : Element(std::move(id)), options_(std::move(options)), value_(value)
    {
        setStretch({1, 0});
    }
    Choice::Choice(Pane& parent, ElementId id, std::vector<ChoiceOption> options, std::int64_t value)
        : Element(parent, std::move(id)), options_(std::move(options)), value_(value)
    {
        setStretch({1, 0});
    }
    Choice::Choice(Element& parent, ElementId id, std::vector<ChoiceOption> options, std::int64_t value)
        : Element(parent, std::move(id)), options_(std::move(options)), value_(value)
    {
        setStretch({1, 0});
    }
    void Choice::setOptions(std::vector<ChoiceOption> options) noexcept
    {
        requireOwner(*this);
        options_ = std::move(options);
        if (std::ranges::find(options_, value_, &ChoiceOption::value) == options_.end())
            value_ = options_.empty() ? -1 : options_.front().value;
    }
    void Choice::setValue(std::int64_t value) noexcept
    {
        requireOwner(*this);
        value_ = value;
    }
    SizeHint Choice::sizeHintContent() noexcept
    {
        float width{40};
        for (const auto& option : options_)
            width = std::max(width, textSize(option.label).width + 32);
        return fieldHint(width);
    }
    void Choice::draw() noexcept
    {
        const auto current =
            std::find_if(options_.begin(), options_.end(), [&](const auto& option) { return option.value == value_; });
        ImGui::SetNextItemWidth(rect().size.width);
        if (!ImGui::BeginCombo("##value", current == options_.end() ? "" : current->label.c_str()))
            return;
        for (std::size_t i{}; i < options_.size(); ++i)
        {
            const auto& option = options_[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(option.label.c_str(), value_ == option.value) && value_ != option.value)
            {
                value_ = option.value;
                static_cast<void>(emit(edited, EditResult{true, true, true, false}));
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}
