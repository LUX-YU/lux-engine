#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/ui/detail/DragDropEncoding.hpp>

namespace lux::ui
{
    ImageElement::ImageElement() : Element() {}

    void ImageElement::setImage(rdesc::UiImageToken image) noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        image_ = image;
    }

    void ImageElement::setUv(Vec2 minimum, Vec2 maximum) noexcept
    {
        const bool is_invalid_uv = !std::isfinite(minimum.x) || !std::isfinite(minimum.y) ||
                                   !std::isfinite(maximum.x) || !std::isfinite(maximum.y);
        if (!isOnAffinityThread() || is_invalid_uv)
        {
            detail::failContract();
        }
        uv_min_ = minimum;
        uv_max_ = maximum;
    }

    void ImageElement::setImageExtent(Size physical_pixels) noexcept
    {
        const bool invalid = !std::isfinite(physical_pixels.width) || !std::isfinite(physical_pixels.height) ||
                             physical_pixels.width < 0 || physical_pixels.height < 0;
        if (!isOnAffinityThread() || invalid)
        {
            detail::failContract();
        }
        image_extent_ = physical_pixels;
    }

    void ImageElement::setSize(Size size) noexcept
    {
        const bool is_invalid_size =
            !std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 0 || size.height < 0;
        if (!isOnAffinityThread() || is_invalid_size)
        {
            detail::failContract();
        }
        size_ = size;
    }

    SizeHint ImageElement::sizeHintContent() noexcept
    {
        SizeHint hint;
        hint.preferred = {size_.width > 0 ? size_.width : 320.F, size_.height > 0 ? size_.height : 200.F};
        return hint;
    }

    void ImageElement::draw() noexcept
    {
        const auto available = rect().size;
        auto origin = ImGui::GetCursorScreenPos();
        const auto previous_size = interaction_.size;
        interaction_ = {};
        const auto framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
        interaction_.framebuffer_scale = {framebuffer_scale.x, framebuffer_scale.y};
        interaction_.available_size = available;
        interaction_.size = available;
        if (image_extent_.width > 0 && image_extent_.height > 0)
        {
            const float aspect =
                image_extent_.width * framebuffer_scale.y / (image_extent_.height * framebuffer_scale.x);
            if (available.height > 0 && available.width / available.height > aspect)
            {
                interaction_.size.width = available.height * aspect;
            }
            else
            {
                interaction_.size.height = available.width / aspect;
            }
            origin.x += (available.width - interaction_.size.width) * 0.5F;
            origin.y += (available.height - interaction_.size.height) * 0.5F;
            ImGui::SetCursorScreenPos(origin);
        }
        interaction_.content_origin = {origin.x, origin.y};
        interaction_.resized = previous_size != interaction_.size;
        if (interaction_.size.width <= 0 || interaction_.size.height <= 0)
        {
            return;
        }
        const ImVec2 extent{interaction_.size.width, interaction_.size.height};
        if (image_.value != 0)
        {
            ImGui::Image(
                static_cast<ImTextureID>(image_.value),
                extent,
                {uv_min_.x, uv_min_.y},
                {uv_max_.x, uv_max_.y}
            );
        }
        else
        {
            ImGui::Dummy(extent);
        }
        interaction_.hovered = ImGui::IsItemHovered();
        interaction_.window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const auto pointer = ImGui::GetIO().MousePos;
        interaction_.local_pointer = {pointer.x - origin.x, pointer.y - origin.y};
        interaction_.left_clicked = interaction_.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        interaction_.middle_clicked = interaction_.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        interaction_.right_clicked = interaction_.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        interaction_.drop = detail::acceptDragDropPayload();
    }
} // namespace lux::ui
