#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/ui/SceneViewElement.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>   // encodeSceneViewSentinel + lux::render::RenderSceneId
#include <imgui.h>

#include <cstdint>
#include <functional>

namespace lux::ui
{
    /**
     * @brief A SceneViewElement specialized for an interactive (orbitable) live view.
     *
     * is-a SceneViewElement: it displays a render view's offscreen color image
     * (via the sentinel texture id) exactly like the base, and additionally
     * captures orbit-camera INPUT over that image, forwarding it as deltas through
     * a callback. It owns NO render scene and does NO session I/O — the host binds
     * it to a view (`setView`) and routes its orbit/resize callbacks to whatever
     * drives that view (e.g. `lux::editor::PreviewScene`). This keeps the element
     * reusable for any live, rotatable preview (material now; mesh/model later).
     *
     * Use within a Panel's frame-CLOSED paint():
     * @code
     *   void MyPanel::paint() override { preview_.draw(); }
     * @endcode
     */
    class LUX_FUNCTION_UI_PUBLIC PreviewViewElement : public SceneViewElement
    {
    public:
        explicit PreviewViewElement(std::string label = "Preview")
            : SceneViewElement(std::move(label)) {}

        /// Bind the element to the live offscreen view it should display. The
        /// view's color image is resolved each UI tick from the sentinel id.
        void setView(lux::render::RenderSceneId scene_id, std::uint32_t view_id) noexcept
        {
            setTextureID(encodeSceneViewSentinel(scene_id, view_id));
        }

        /// (d_yaw, d_pitch, d_zoom) in the camera's own units (radians-ish, zoom
        /// factor). Fired while the image is hovered + dragged / wheeled.
        using OrbitCallback = std::function<void(float d_yaw, float d_pitch, float d_zoom)>;
        void setOrbitCallback(OrbitCallback cb) { orbit_cb_ = std::move(cb); }

        /// Drag/wheel sensitivity (image-space pixels -> camera delta).
        void setOrbitSpeed(float radians_per_pixel) noexcept { orbit_speed_ = radians_per_pixel; }
        void setZoomSpeed(float factor_per_notch)   noexcept { zoom_speed_  = factor_per_notch; }

        void draw() override
        {
            // Remember where the base places the image, then let it draw the
            // image (ImGui::Image) + aspect letterbox + fire the resize callback.
            const ImVec2 img_pos = ImGui::GetCursorScreenPos();
            SceneViewElement::draw();
            const ImVec2 img_size = contentSize();

            if (!orbit_cb_ || textureID() == 0 || img_size.x <= 0.f || img_size.y <= 0.f)
                return;

            // Overlay an invisible button over the just-drawn image to RELIABLY
            // capture dragging: IsItemActive() stays true for the whole LMB drag,
            // whereas IsItemHovered() drops as the cursor leaves the rect mid-drag.
            // (The standard draggable-image-viewport pattern.)
            ImGui::SetCursorScreenPos(img_pos);
            ImGui::InvisibleButton("##orbit", img_size);
            const bool active  = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();

            const ImGuiIO& io = ImGui::GetIO();
            float d_yaw = 0.f, d_pitch = 0.f, d_zoom = 0.f;
            if (active)
            {
                d_yaw   = io.MouseDelta.x * orbit_speed_;
                d_pitch = io.MouseDelta.y * orbit_speed_;
            }
            if (hovered && io.MouseWheel != 0.f)
                d_zoom = io.MouseWheel * zoom_speed_;

            if (d_yaw != 0.f || d_pitch != 0.f || d_zoom != 0.f)
                orbit_cb_(d_yaw, d_pitch, d_zoom);
        }

    private:
        OrbitCallback orbit_cb_;
        float         orbit_speed_{0.01f};
        float         zoom_speed_{0.15f};
    };

} // namespace lux::ui
