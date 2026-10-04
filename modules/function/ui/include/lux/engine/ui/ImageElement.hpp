#pragma once

#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/DragDrop.hpp>
#include <optional>

namespace lux::ui
{
    struct ImageInteraction final
    {
        Size size;
        Point content_origin, local_pointer;
        bool hovered{}, window_focused{}, resized{};
        bool left_clicked{}, middle_clicked{}, right_clicked{};
        // A frame-borrowed payload. Decode/copy needed business values during draw;
        // never retain this view in queued work or a navigation request.
        std::optional<DragDropPayloadView> drop;
    };

    // Displays an already resolved image. The caller retains the resource until
    // the root has captured every draw that references it; this Pane owns no GPU use.
    class LUX_FUNCTION_PUBLIC ImageElement : public Element
    {
    public:
        ImageElement(object::ObjectDispatcherRef dispatcher, ElementId id);
        ImageElement(Pane& parent, ElementId id);
        ImageElement(Element& parent, ElementId id);

        void setImage(render::RTextureHandle image) noexcept;
        [[nodiscard]] render::RTextureHandle image() const noexcept
        {
            return image_;
        }
        void setUv(Vec2 minimum, Vec2 maximum) noexcept;
        // Preferred size for layout. Zero uses the default hint; arrangement owns the displayed extent.
        void setSize(Size size) noexcept;
        [[nodiscard]] Size displayedSize() const noexcept
        {
            return interaction_.size;
        }
        [[nodiscard]] Point contentOrigin() const noexcept
        {
            return interaction_.content_origin;
        }
        // Geometry is from the most recent draw; drop bytes remain frame-borrowed.
        [[nodiscard]] const ImageInteraction& interaction() const noexcept
        {
            return interaction_;
        }

    protected:
        SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;

    private:
        render::RTextureHandle image_;
        Vec2 uv_min_{};
        Vec2 uv_max_{1.0F, 1.0F};
        Size size_{};
        ImageInteraction interaction_;
    };
} // namespace lux::ui
