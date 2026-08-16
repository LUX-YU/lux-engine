#include <lux/engine/ui/UIOffscreenImagePool.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>

#include <imgui_impl_vulkan.h>

#include <cstdint>

namespace lux::ui
{
    void UIOffscreenImagePool::setImGuiRenderer(ImGui_ImplVulkan_Renderer *renderer) noexcept
    {
        if (imgui_renderer_ == renderer)
            return;

        VkDevice device = res_ctx_.deviceContext().logicalDevice();
        if (imgui_renderer_)
        {
            for (VkDescriptorSet ds : imgui_descriptors_)
                if (ds != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTextureEx(imgui_renderer_, ds);

            for (auto &retired : retired_imgui_)
            {
                for (VkDescriptorSet ds : retired.descriptors)
                    if (ds != VK_NULL_HANDLE)
                        ImGui_ImplVulkan_RemoveTextureEx(imgui_renderer_, ds);
                if (retired.sampler != VK_NULL_HANDLE)
                    vkDestroySampler(device, retired.sampler, nullptr);
            }
            retired_imgui_.clear();
        }

        if (imgui_sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device, imgui_sampler_, nullptr);
        imgui_descriptors_.clear();
        imgui_sampler_ = VK_NULL_HANDLE;
        imgui_renderer_ = renderer;
    }

    UIOffscreenImagePool::~UIOffscreenImagePool()
    {
        retireImGuiResources();
        if (imgui_renderer_)
        {
            for (auto &retired : retired_imgui_)
            {
                for (VkDescriptorSet ds : retired.descriptors)
                    if (ds != VK_NULL_HANDLE)
                        ImGui_ImplVulkan_RemoveTextureEx(imgui_renderer_, ds);
                if (retired.sampler != VK_NULL_HANDLE)
                {
                    VkDevice device = res_ctx_.deviceContext().logicalDevice();
                    vkDestroySampler(device, retired.sampler, nullptr);
                }
            }
        }
        retired_imgui_.clear();
        // ~OffscreenImagePool handles image/view cleanup
    }

    void UIOffscreenImagePool::ensureImGuiDescriptors()
    {
        if (!imgui_descriptors_.empty())
            return;

        VkDevice device = res_ctx_.deviceContext().logicalDevice();

        // Sampler shared by all FIF descriptors.
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &sci, nullptr, &imgui_sampler_);

        // One descriptor per FIF slot, permanently bound to its colour view.
        auto &color_views = slot_views_[static_cast<size_t>(
            lux::render::TargetSlot::SCENE_COLOR)];

        imgui_descriptors_.resize(color_views.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < color_views.size(); ++i)
        {
            imgui_descriptors_[i] = ImGui_ImplVulkan_AddTextureEx(
                imgui_renderer_, imgui_sampler_, color_views[i],
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    void UIOffscreenImagePool::resize(VkExtent2D new_extent)
    {
        // Old images are retired by OffscreenImagePool::resize() and GC'd later.
        retireImGuiResources();
        OffscreenImagePool::resize(new_extent);
        // Descriptors will be lazily recreated on next ensureImGuiDescriptors().
    }

    void UIOffscreenImagePool::collectRetired(uint64_t frame_id, uint64_t completed_serial)
    {
        OffscreenImagePool::collectRetired(frame_id, completed_serial);

        if (!imgui_renderer_ || retired_imgui_.empty())
            return;

        auto it = retired_imgui_.begin();
        while (it != retired_imgui_.end())
        {
            if (it->retire_frame == 0)
            {
                it->retire_frame = frame_id;
                ++it;
                continue;
            }

            // Fence-proven watermark, not serial arithmetic (see base class).
            const bool aged_out = (it->retire_frame <= completed_serial);
            if (!aged_out)
            {
                ++it;
                continue;
            }

            for (VkDescriptorSet ds : it->descriptors)
                if (ds != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RemoveTextureEx(imgui_renderer_, ds);

            if (it->sampler != VK_NULL_HANDLE)
            {
                VkDevice device = res_ctx_.deviceContext().logicalDevice();
                vkDestroySampler(device, it->sampler, nullptr);
            }

            it = retired_imgui_.erase(it);
        }
    }

    void UIOffscreenImagePool::retireImGuiResources()
    {
        if (imgui_descriptors_.empty() && imgui_sampler_ == VK_NULL_HANDLE)
            return;

        RetiredImGuiResources retired;
        retired.descriptors = std::move(imgui_descriptors_);
        retired.sampler = imgui_sampler_;
        retired.retire_frame = 0;
        retired_imgui_.push_back(std::move(retired));

        imgui_descriptors_.clear();
        imgui_sampler_ = VK_NULL_HANDLE;
    }
} // namespace lux::ui
