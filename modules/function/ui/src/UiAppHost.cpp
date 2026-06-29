#include <lux/engine/ui/UiAppHost.hpp>

#include <lux/engine/ui/UISystem.hpp>          // requiredVulkanExtensions
#include <lux/engine/ui/ImGuiCommConfig.hpp>   // ImGuiCommConfig / ETextureFormatHint

#include <imgui.h>

#include <cstdio>
#include <utility>

namespace lux::ui
{
    std::unique_ptr<UIRenderServer> bringUpUIRenderServer(
        std::shared_ptr<lux::render::RenderProgramChannel<>> channel,
        std::shared_ptr<lux::render::RenderChannelSync>      sync,
        lux::window::LuxWindow&                              window,
        bool                                                 enable_validation,
        const char*                                          log_tag)
    {
        auto server = std::make_unique<UIRenderServer>(std::move(channel), std::move(sync));

        lux::render::ServerConfig cfg;
        cfg.enable_validation = enable_validation;
        for (auto* ext : UISystem::requiredVulkanExtensions())
            cfg.instance_extensions.emplace_back(ext);

        // Font atlas pixels — the ImGui context was created by UISystem on the
        // main thread, so reading the atlas here (on the render thread) is safe:
        // it is fully built before this thread first touches the GPU.
        ImGuiCommConfig imgui_cfg{};
        imgui_cfg.color_format = lux::render::ETextureFormatHint::SBGRA8;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
            &imgui_cfg.font_pixels, &imgui_cfg.font_width, &imgui_cfg.font_height);

        if (auto r = server->init(std::move(cfg), imgui_cfg); !r)
        {
            std::fprintf(stderr, "[%s] UIRenderServer::init failed\n", log_tag);
            return nullptr;
        }
        if (auto r = server->attachToWindow(window); !r)
        {
            std::fprintf(stderr, "[%s] UIRenderServer::attachToWindow failed\n", log_tag);
            return nullptr;
        }

        return server;
    }

} // namespace lux::ui
