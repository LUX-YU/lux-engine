#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/detail/ImageEncoding.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/ui/detail/DragDropEncoding.hpp>
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace lux::ui
{
    ImageElement::ImageElement(object::ObjectDispatcherRef dispatcher, ElementId id)
        : Element(std::move(dispatcher), std::move(id))
    {}
    ImageElement::ImageElement(Pane& parent, ElementId id) : Element(parent, std::move(id)) {}
    ImageElement::ImageElement(Element& parent, ElementId id) : Element(parent, std::move(id)) {}

    void ImageElement::setImage(render::RTextureHandle image) noexcept
    {
        if (!isOnAffinityThread())
            detail::failContract();
        image_ = image;
    }

    void ImageElement::setUv(Vec2 minimum, Vec2 maximum) noexcept
    {
        const bool is_invalid_uv = !std::isfinite(minimum.x) || !std::isfinite(minimum.y) ||
                                   !std::isfinite(maximum.x) || !std::isfinite(maximum.y);
        if (!isOnAffinityThread() || is_invalid_uv)
            detail::failContract();
        uv_min_ = minimum;
        uv_max_ = maximum;
    }

    void ImageElement::setSize(Size size) noexcept
    {
        const bool is_invalid_size =
            !std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 0 || size.height < 0;
        if (!isOnAffinityThread() || is_invalid_size)
            detail::failContract();
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
        const auto origin = ImGui::GetCursorScreenPos();
        const auto previous_size = interaction_.size;
        interaction_ = {};
        interaction_.content_origin = {origin.x, origin.y};
        interaction_.size = available;
        interaction_.resized = previous_size != interaction_.size;
        if (interaction_.size.width <= 0 || interaction_.size.height <= 0)
            return;
        const ImVec2 extent{interaction_.size.width, interaction_.size.height};
        if (image_.isValid())
        {
            ImGui::Image(
                static_cast<ImTextureID>(detail::encodeImage(image_)),
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
