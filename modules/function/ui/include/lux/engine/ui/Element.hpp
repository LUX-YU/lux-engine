#pragma once

#include <limits>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/Ids.hpp>

namespace lux::ui
{
    class Root;
    class Pane;

    struct SizeHint final
    {
        Size minimum{};
        Size preferred{};
        Size maximum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    };

    enum class EAlignment : std::uint8_t
    {
        START,
        CENTER,
        END,
        FILL
    };

    // Content identity and interaction; never a window or docking participant.
    class LUX_FUNCTION_PUBLIC Element : public lux::object::LuxObject
    {
    public:
        Element(Pane& parent, ElementId id);
        Element(Element& parent, ElementId id);
        ~Element() noexcept override;
        [[nodiscard]] Root& root() const noexcept;
        [[nodiscard]] Pane& pane() const noexcept;
        [[nodiscard]] const ElementId& id() const noexcept
        {
            return id_;
        }
        [[nodiscard]] bool visible() const noexcept
        {
            return visible_;
        }
        [[nodiscard]] bool enabled() const noexcept
        {
            return enabled_;
        }
        [[nodiscard]] bool displayed() const noexcept;
        [[nodiscard]] bool focused() const noexcept;
        [[nodiscard]] bool hovered() const noexcept
        {
            return hovered_;
        }
        [[nodiscard]] Rect rect() const noexcept
        {
            return rect_;
        }
        [[nodiscard]] Point contentOrigin() const noexcept
        {
            return draw_origin_;
        }
        [[nodiscard]] Vec2 stretch() const noexcept
        {
            return stretch_;
        }
        [[nodiscard]] EAlignment horizontalAlignment() const noexcept
        {
            return horizontal_alignment_;
        }
        [[nodiscard]] EAlignment verticalAlignment() const noexcept
        {
            return vertical_alignment_;
        }

        virtual void finishEdit(bool cancel = false) noexcept {}
        void setVisible(bool visible) noexcept;
        void setEnabled(bool enabled) noexcept;
        void setMinimumSize(Size size) noexcept;
        void setMaximumSize(Size size) noexcept;
        void setStretch(Vec2 weight) noexcept;
        void setAlignment(EAlignment horizontal, EAlignment vertical) noexcept;
        [[nodiscard]] SizeHint sizeHint() noexcept;
        [[nodiscard]] SizeHint measure(float width) noexcept;
        void arrange(Rect rect) noexcept;

    protected:
        [[nodiscard]] virtual SizeHint sizeHintContent() noexcept;
        [[nodiscard]] virtual SizeHint measureContent(float width) noexcept;
        virtual void arrangeContent() noexcept {}
        virtual void draw() noexcept = 0;
        virtual void update() noexcept {}
        void drawChild(Element& child, Point offset = {}) noexcept;

    private:
        friend class Root;
        friend class Pane;
        bool allowsGenericChildren() const noexcept override
        {
            return false;
        }
        Element(object::LuxObject&, Pane&, Element*, ElementId);
        [[nodiscard]] SizeHint constrain(SizeHint) const noexcept;
        std::size_t registration_slot_{SIZE_MAX};
        std::uint64_t hint_epoch_{}, measure_epoch_{};
        SizeHint intrinsic_hint_, measured_hint_;
        float measured_width_{};
        Element* element_parent_{};
        ElementId id_;
        Pane* pane_{};
        Rect rect_;
        Point draw_origin_;
        Size minimum_{};
        Size maximum_{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        Vec2 stretch_{1.F, 1.F};
        EAlignment horizontal_alignment_{EAlignment::FILL}, vertical_alignment_{EAlignment::FILL};
        bool visible_{true}, enabled_{true}, hovered_{};
    };
}
