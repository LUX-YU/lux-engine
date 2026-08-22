#pragma once
// ============================================================================
//  UIRenderServer — GeneralRenderServer subclass with ImGui support
//
//  Adds:
//  - handleAddUIView / handleRemoveUIView handlers (creates UIOffscreenTarget)
//  - SubmitImGuiDrawData handler (stashes snapshotted draw data)
//  - Eager ImGui scene/view init in init()
//  - tick() override: offscreen render → update descriptors → swapchain/ImGui render
// ============================================================================

#include <lux/engine/math/Extent.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/function/visibility_ui.h>

#include <cstdint>

namespace lux::render { class Renderer; }

namespace lux::ui
{
    struct ImGuiCommConfig;

    class LUX_FUNCTION_UI_PUBLIC UIRenderServer : public lux::render::GeneralRenderServer
    {
    public:
        UIRenderServer(
            std::shared_ptr<Channel> frame_channel,
            std::shared_ptr<lux::render::RenderControlChannel<>> control_channel,
            std::shared_ptr<lux::render::RenderUploadChannel<>> upload_channel,
            std::shared_ptr<lux::render::RenderChannelSync> sync);

        ~UIRenderServer() override;

        /// Initialize with ImGui configuration. Creates the ImGui overlay
        /// scene/view/feature eagerly — no lazy binding needed.
        [[nodiscard]] lux::render::Expected<void> init(
            lux::render::ServerConfig config,
            const ImGuiCommConfig& imgui_config
        );

        /// Attach to window — creates swapchain target then eagerly
        /// creates the ImGui swapchain view (instead of lazy-creating in tick).
        [[nodiscard]] lux::render::Expected<void> attachToWindow(lux::window::LuxWindow& window);

        /// ImGui operation IDs (valid after init()).
        [[nodiscard]] ImGuiOperationIds imguiOps() const noexcept;

        /// tick() override: drain → render offscreen → swapchain scene → swapchain ImGui.
        bool tick() override;

        // ─── Swapchain scene rendering ───────────────────────────────────

        /// Designate a scene to render directly to the swapchain surface.
        /// The scene must already exist (created via createScene).
        /// A view is created internally and the scene's graph is compiled
        /// against the swapchain layout (with depth managed by the render graph).
        /// Only one swapchain scene is supported at a time; call
        /// clearSwapchainScene() first to replace.
        /// @return The view handle for the created swapchain view (use with
        ///         session->updateView() to set camera data), or an invalid
        ///         handle on failure.
        [[nodiscard]] lux::render::ViewHandle setSwapchainScene(lux::render::RenderSceneId scene_id);

        /// Remove the current swapchain scene.  The internal view is destroyed.
        /// Safe to call when no swapchain scene is set (no-op).
        void clearSwapchainScene();

        // (服务端直呼 addUIView / removeUIView 已消亡:UI 目标统一走
        //  CreateOffscreenTarget(SAMPLED) + SetLayer 核心命令面。)

        /// Enable or disable ImGui overlay rendering on top of the swapchain.
        /// When disabled, only the swapchain scene (if any) renders to the
        /// swapchain surface.  Default: true.
        void setImGuiOverlayEnabled(bool enabled);

    public:
        struct UIState;
    private:
        std::unique_ptr<UIState> ui_;
    };

} // namespace lux::ui
