#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility_ui.h>

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <string>

struct ImGui_ImplVulkan_Renderer;

namespace lux::ui
{
    /// Font pixel data captured on the game thread and read once by the render thread.
    struct ImGuiInitConfig
    {
        VkFormat       color_format{VK_FORMAT_B8G8R8A8_UNORM};
        unsigned char* font_pixels{nullptr};
        int            font_width{0};
        int            font_height{0};
        std::string    color_target{"SceneColor"};
    };

    /**
     * @brief RenderFeature that renders snapshotted ImGui draw data as a UI overlay.
     *
     * The game thread exclusively owns the ImGuiContext and produces draw lists.
     * The render thread exclusively owns an ImGui_ImplVulkan_Renderer (Ex API)
     * and calls RenderDrawDataEx — no ImGuiContext dependency.
     *
     * Sentinel texture IDs are resolved inline by the TexResolver callback
     * registered on the Ex renderer (see UIRenderServer).
     */
    class LUX_FUNCTION_UI_PUBLIC ImGuiFeature : public lux::render::RenderFeature
    {
    public:
        explicit ImGuiFeature(VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM,
                              unsigned char* font_pixels = nullptr,
                              int font_width = 0, int font_height = 0);
                              
        ~ImGuiFeature() override;

        // ── Render-thread API (used by UIRenderServer) ────────────────────

        /// Provide a snapshotted ImDrawData for the next render pass.
        void setDrawData(ImDrawData* data) { pending_draw_data_ = data; }

        /// Set the Ex renderer handle. Owned by UIRenderServer, not by this feature.
        void setVkRenderer(ImGui_ImplVulkan_Renderer* r) { vk_renderer_ = r; }

        /// Access the init config (font pixel data for lazy renderer creation).
        [[nodiscard]] const ImGuiInitConfig& initConfig() const noexcept { return init_config_; }

        // ── RenderFeature overrides ──────────────────────────────────────────

        std::string_view name()     const override { return "ImGuiOverlay"; }

        lux::render::Expected<void> initAndAttachTo(lux::render::RenderScene& scene) override;
        void onDetachFromScene(lux::render::RenderScene& scene) override;

        void addPasses(lux::render::RGBuilder& builder) override;

    private:
        ImGuiInitConfig            init_config_;
        ImDrawData*                pending_draw_data_{nullptr};
        ImGui_ImplVulkan_Renderer* vk_renderer_{nullptr};
    };

} // namespace lux::ui
