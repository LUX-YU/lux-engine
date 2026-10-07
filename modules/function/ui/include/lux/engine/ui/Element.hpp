#pragma once

#include <limits>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/PaneError.hpp>

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
        Element() noexcept = default;
        [[nodiscard]] Root* attachedRoot() const noexcept;
        [[nodiscard]] Pane* containingPane() const noexcept
        {
            return pane_;
        }
        ~Element() noexcept override;
        [[nodiscard]] Root& root() const noexcept;
        [[nodiscard]] Pane& pane() const noexcept;
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

        [[nodiscard]] PaneResult<void> addElement(Element&) noexcept;
        [[nodiscard]] PaneResult<void> replaceElement(Element& previous, Element&) noexcept;

    protected:
        // Only composites override this; leaf controls reject children even via a base reference.
        [[nodiscard]] virtual bool acceptsElements() const noexcept
        {
            return false;
        }
        void clearElements() noexcept;
        [[nodiscard]] virtual SizeHint sizeHintContent() noexcept;
        [[nodiscard]] virtual SizeHint measureContent(float width) noexcept;
        virtual void arrangeContent() noexcept {}
        virtual void draw() noexcept = 0;
        virtual void update() noexcept {}
        void drawChild(Element& child, Point offset = {}) noexcept;

    private:
        using LuxObject::addChild;
        using LuxObject::removeChild;
        using LuxObject::setParent;
        friend class Root;
        friend class Pane;
        friend class Layout;
        bool allowsGenericStructure() const noexcept final
        {
            return false;
        }
        void assignPane(Pane*) noexcept;
        [[nodiscard]] SizeHint constrain(SizeHint) const noexcept;
        std::uint64_t hint_epoch_{}, measure_epoch_{};
        SizeHint intrinsic_hint_, measured_hint_;
        float measured_width_{};
        Element* element_parent_{};
        Pane* pane_{};
        Rect rect_;
        Point draw_origin_;
        Size minimum_{};
        Size maximum_{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        Vec2 stretch_{1.F, 1.F};
        EAlignment horizontal_alignment_{EAlignment::FILL}, vertical_alignment_{EAlignment::FILL};
        bool visible_{true}, enabled_{true}, hovered_{};
    };
} // namespace lux::ui
