#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/ui/AssetDragDrop.hpp>
#include <lux/engine/ui/Panel.hpp>

#include <lux/cxx/event/Signal.hpp>   // resized / picked / asset_dropped

#include <imgui.h>

#include <cstdint>

namespace lux::ui
{
    /// Payload of SceneViewportPanel::resized — new content region size in pixels.
    struct ViewportResized
    {
        uint32_t width;
        uint32_t height;
    };

    /// Payload of SceneViewportPanel::picked — a clean LMB click-release with no
    /// camera interaction. Coordinates are content-rect-local pixels (top-left =
    /// 0,0); width/height mirror the content size at the click.
    struct ViewportPicked
    {
        float content_x;
        float content_y;
        float content_w;
        float content_h;
    };

    /// Payload of SceneViewportPanel::asset_dropped — an AssetBrowser drag-source
    /// carrying kAssetDragPayloadTag was released over the image. Drop position is
    /// content-rect-local pixels. The payload is carried BY VALUE (POD memcpy).
    struct ViewportAssetDropped
    {
        AssetDragPayload payload;
        float            drop_x;
        float            drop_y;
        float            content_w;
        float            content_h;
    };

    /**
     * @brief Widget that displays a Vulkan texture (e.g. an offscreen render target)
     *        inside an ImGui panel via ImGui::Image.
     *
     * Usage:
     *   - Call setTextureID() with a valid ImTextureID obtained from
     *     ImGui_ImplVulkan_AddTexture() or ImGuiFeature::registerTexture().
     *   - The widget automatically respects the ImGui panel's available size.
     *   - Call contentSize() after paint to retrieve the actual displayed size
     *     (useful for detecting resize and updating offscreen targets).
     */
    class LUX_FUNCTION_UI_PUBLIC SceneViewportPanel : public Panel
    {
    public:
        explicit SceneViewportPanel(std::string title = "Scene Viewport",
                                    std::array<float, 2> suggest_size = {640, 480});
        ~SceneViewportPanel() override;

        /// Set the ImTextureID to display (thread-safe: atomic store).
        void setTextureID(ImTextureID id) noexcept;

        /// Get the current ImTextureID.
        ImTextureID textureID() const noexcept;

        /// Aspect ratio of the displayed image (0 = stretch to fit).
        void setAspectRatio(float ratio) noexcept { aspect_ratio_ = ratio; }
        float aspectRatio() const noexcept { return aspect_ratio_; }

        /// After paint(), returns the actual content region size (in pixels).
        ImVec2 contentSize() const noexcept { return content_size_; }

        // ── Viewport signals (replace the old std::function callbacks) ──────
        //
        // `resized`       — content region size changed.
        // `picked`        — a clean LMB click-release with NO camera interaction
        //                   (see the pick-disambiguation block in paint()).
        // `asset_dropped` — an AssetBrowser drag-source carrying
        //                   kAssetDragPayloadTag was released over the image.
        //
        // Signals (not single callbacks): any number of hosts may connect, and
        // emitting with no subscribers is a harmless no-op. The host owns the
        // connections; their lifetime must not outlive this panel.
        lux::cxx::event::Signal<ViewportResized>      resized;
        lux::cxx::event::Signal<ViewportPicked>       picked;
        lux::cxx::event::Signal<ViewportAssetDropped> asset_dropped;

        // ── M2: pointer / capture hooks for picking + UE-style camera ─────

        /// Per-frame pointer state over the rendered image area. Refreshed
        /// inside paint(); read by host code (LuxEditor::run, EditorScene::tick)
        /// to drive viewport-anchored input (pick on click, RMB-fly camera, ...).
        ///
        /// `pos_in_content` is in content-rect pixels (top-left = 0,0). It is
        /// only meaningful when `hovered` is true. `content_size` mirrors
        /// contentSize() for callers that want a single snapshot.
        struct ViewportPointer
        {
            bool   hovered{false};
            bool   focused{false};
            ImVec2 pos_in_content{0, 0};
            ImVec2 content_size{0, 0};
        };

        /// Pointer state captured during the last paint() call. Valid after
        /// the panel has been painted at least once.
        [[nodiscard]] ViewportPointer pointer() const noexcept { return pointer_; }

        /// True when the host explicitly wants the panel to keep mouse input
        /// inside the viewport (e.g. RMB-fly camera is active). The host sets
        /// this each frame from outside ImGui logic — see LuxEditor::run.
        ///
        /// Currently informational only; the host is responsible for poking
        /// `ImGuiIO::WantCaptureMouse` / cursor mode when this is true.
        [[nodiscard]] bool ownsMouse() const noexcept { return owns_mouse_; }
        void setOwnsMouse(bool b) noexcept { owns_mouse_ = b; }

    protected:
        void paint() override;

    private:
        ImTextureID    texture_id_{0};
        float          aspect_ratio_{0.f};
        ImVec2         content_size_{0, 0};
        ImVec2         prev_content_size_{0, 0};

        ViewportPointer   pointer_{};
        bool              owns_mouse_{false};
        // Pick-vs-camera disambiguation state (see the "Pick on click" block in
        // paint()). A pick must be a clean LMB click with NO camera interaction:
        bool              prev_owns_mouse_{false};  ///< edge-detect the end of camera ownership (RMB fly)
        int               pick_cooldown_{0};        ///< frames to keep suppressing pick after ownership ends
        bool              lmb_drag_latched_{false}; ///< this LMB hold passed the drag threshold → it's an orbit, not a click
    };

} // namespace lux::ui
