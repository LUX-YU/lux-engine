#include <lux/engine/ui/ImGuiFeature.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>

#include <imgui_impl_vulkan.h>

#include <cstdio>
namespace lux::ui
{
    ImGuiFeature::ImGuiFeature(VkFormat color_format, unsigned char* font_pixels, int font_width, int font_height)
        : init_config_{color_format, font_pixels, font_width, font_height}
    {
    }

    ImGuiFeature::~ImGuiFeature() = default;

    lux::render::Expected<void> ImGuiFeature::initAndAttachTo(lux::render::RenderScene & /*scene*/)
    {
        return {};
    }

    void ImGuiFeature::onDetachFromScene(lux::render::RenderScene & /*scene*/)
    {
        vk_renderer_ = nullptr;
    }

    void ImGuiFeature::addPasses(lux::render::RGBuilder &builder)
    {
        builder.addPass("ImGuiOverlay", lux::render::ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(init_config_.color_target), ::lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setRecorder(
                [this](const lux::render::PassRecordContext &ctx)
                {
                    ImDrawData* data = pending_draw_data_;

                    if (vk_renderer_ && data && data->Valid && data->TotalIdxCount > 0){
                        ImGui_ImplVulkan_RenderDrawDataEx(vk_renderer_, data, ctx.cmd);
                    }

                    pending_draw_data_ = nullptr; 
                }
            )
            .withPhase(static_cast<lux::render::render_phase_id>(lux::render::ECoreRenderPhase::UI));
    }

} // namespace lux::ui
