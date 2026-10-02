#pragma once

#include <lux/engine/ui/Element.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace lux::ui
{
    struct EditResult final
    {
        bool changed{};
        bool began{};
        bool committed{};
        bool cancelled{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept
        {
            return changed;
        }
    };

    enum class EScalarEditMode : std::uint8_t
    {
        INPUT,
        DRAG,
        SLIDER,
    };

    class LUX_FUNCTION_PUBLIC Button final : public Element
    {
        friend struct ControlsTestAccess;

    public:
        object::TSignal<> activated{*this};
        Button(Pane& parent, ElementId id, std::string text);
        Button(Element& parent, ElementId id, std::string text);
        void setText(std::string text);

    private:
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        std::string text_, label_;
    };
    class LUX_FUNCTION_PUBLIC Label final : public Element
    {
    public:
        Label(Pane& parent, ElementId id, std::string text = {});
        Label(Element& parent, ElementId id, std::string text = {});
        void setText(std::string text);
        void setWrap(bool wrap) noexcept;

    private:
        SizeHint sizeHintContent() noexcept override;
        SizeHint measureContent(float width) noexcept override;
        void draw() noexcept override;
        std::string text_;
        bool wrap_{};
    };
    class LUX_FUNCTION_PUBLIC CheckBox final : public Element
    {
        friend struct ControlsTestAccess;

    public:
        object::TSignal<EditResult> edited{*this};
        CheckBox(Pane& parent, ElementId id, std::string text, bool value = false);
        CheckBox(Element& parent, ElementId id, std::string text, bool value = false);
        void setValue(bool value) noexcept;
        [[nodiscard]] bool value() const noexcept
        {
            return value_;
        }

    private:
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        std::string text_, label_;
        bool value_{};
    };
    class LUX_FUNCTION_PUBLIC TextEdit final : public Element
    {
        friend struct ControlsTestAccess;

    public:
        object::TSignal<EditResult> edited{*this};
        TextEdit(Pane& parent, ElementId id, std::string value = {});
        TextEdit(Element& parent, ElementId id, std::string value = {});
        void setValue(std::string value);
        void setHint(std::string hint);
        void finishEdit(bool cancel = false) noexcept override;
        [[nodiscard]] bool editing() const noexcept
        {
            return editing_;
        }
        [[nodiscard]] const std::string& value() const noexcept
        {
            return value_;
        }

    private:
        void event(object::EventView& event) noexcept override;
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        std::string value_, before_, hint_;
        bool editing_{};
        unsigned text_id_{};
        int text_command_{};
        bool can_undo_{}, can_redo_{};
    };
    using VNumericValue = std::variant<std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float, double>;
    struct NumericSpec final
    {
        EScalarEditMode mode{EScalarEditMode::DRAG};
        float speed{0.1F};
        std::optional<VNumericValue> minimum, maximum, step;
        std::string format;
    };
    class LUX_FUNCTION_PUBLIC NumericEdit final : public Element
    {
        friend struct ControlsTestAccess;

    public:
        object::TSignal<EditResult> edited{*this};
        NumericEdit(Pane& parent, ElementId id, VNumericValue value);
        NumericEdit(Element& parent, ElementId id, VNumericValue value);
        void setValue(VNumericValue value) noexcept;
        void finishEdit(bool cancel = false) noexcept override;
        [[nodiscard]] bool editing() const noexcept
        {
            return editing_;
        }
        [[nodiscard]] bool setSpec(NumericSpec spec) noexcept;
        [[nodiscard]] bool valueValid() const noexcept;
        [[nodiscard]] const VNumericValue& value() const noexcept
        {
            return value_;
        }

    private:
        void event(object::EventView& event) noexcept override;
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        VNumericValue value_, before_;
        NumericSpec spec_;
        bool editing_{};
        unsigned text_id_{};
        int text_command_{};
        bool can_undo_{}, can_redo_{};
    };
    struct ChoiceOption final
    {
        std::int64_t value{};
        std::string label;
    };
    class LUX_FUNCTION_PUBLIC Choice final : public Element
    {
        friend struct ControlsTestAccess;

    public:
        object::TSignal<EditResult> edited{*this};
        Choice(Pane& parent, ElementId id, std::vector<ChoiceOption> options, std::int64_t value = 0);
        Choice(Element& parent, ElementId id, std::vector<ChoiceOption> options, std::int64_t value = 0);
        // Owner maintenance replaces candidate values; this never emits an editing signal.
        void setOptions(std::vector<ChoiceOption> options) noexcept;
        void setValue(std::int64_t value) noexcept;
        [[nodiscard]] std::int64_t value() const noexcept
        {
            return value_;
        }

    private:
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        std::vector<ChoiceOption> options_;
        std::int64_t value_{};
    };
}
