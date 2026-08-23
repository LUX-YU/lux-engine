#include <lux/engine/ui/ViewportElement.hpp>

#include <lux/engine/ui/detail/DragDropEncoding.hpp>

namespace lux::ui {
ViewportResult ViewportElement::draw(ImTextureID texture, ImVec2 uv0,
                                     ImVec2 uv1) {
  ViewportResult result;
  result.size = ImGui::GetContentRegionAvail();
  result.content_origin = ImGui::GetCursorScreenPos();
  result.resized =
      result.size.x != previous_size_.x || result.size.y != previous_size_.y;
  previous_size_ = result.size;
  ImGui::Image(texture, result.size, uv0, uv1);
  result.hovered = ImGui::IsItemHovered();
  const auto pointer = ImGui::GetIO().MousePos;
  result.local_pointer = {pointer.x - result.content_origin.x,
                          pointer.y - result.content_origin.y};
  result.left_clicked =
      result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  result.middle_clicked =
      result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
  result.right_clicked =
      result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  result.window_focused =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

  result.drop = detail::acceptDragDropPayload();
  return result;
}
} // namespace lux::ui
