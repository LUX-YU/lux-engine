#pragma once

#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <optional>

namespace lux::ui
{
    struct ImageInteraction final
    {
        Size size;
        Size available_size; // Layout rectangle, before fitting the adopted image aspect.
        // Physical pixels per UI unit from this Root's current FrameInfo, not the layout scale.
        Vec2 framebuffer_scale{1.0F, 1.0F};
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
        ImageElement();

        void setImage(rdesc::UiImageToken image) noexcept;

        [[nodiscard]] rdesc::UiImageToken image() const noexcept
        {
            return image_;
        }

        void setUv(Vec2 minimum, Vec2 maximum) noexcept;
        // Adopted image size in physical pixels. Zero disables aspect fitting. Letterbox bars are not interactive.
        void setImageExtent(Size physical_pixels) noexcept;
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
        rdesc::UiImageToken image_;
        Vec2 uv_min_{};
        Vec2 uv_max_{1.0F, 1.0F};
        Size size_{};
        Size image_extent_{};
        ImageInteraction interaction_;
    };
} // namespace lux::ui
