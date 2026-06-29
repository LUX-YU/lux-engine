#pragma once
// ============================================================================
//  UIOffscreenImagePool — OffscreenImagePool subclass with ImGui descriptors
//
//  Adds a VkDescriptorSet (one per FIF slot) so ImGui can render an offscreen
//  view's colour attachment as an image widget.  Descriptors are created lazily
//  by ensureImGuiDescriptors() and permanently point at their colour view.
// ============================================================================

#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/function/visibility_ui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct ImGui_ImplVulkan_Renderer;

namespace lux::ui
{
    class LUX_FUNCTION_UI_PUBLIC UIOffscreenImagePool : public lux::render::OffscreenImagePool
    {
    public:
        using OffscreenImagePool::OffscreenImagePool;
        ~UIOffscreenImagePool() override;

        /// Set the Ex renderer for thread-safe ImGui descriptor management.
        void setImGuiRenderer(ImGui_ImplVulkan_Renderer *renderer) noexcept;

        // ── ImGui descriptor management ──────────────────────────────────────

        /// Lazy-create sampler + one ImGui descriptor set per FIF colour view.
        /// Safe to call multiple times — only initializes once.
        void ensureImGuiDescriptors();

        /// Returns the ImGui-usable descriptor set for the given FIF slot.
        VkDescriptorSet imguiDescriptor(uint32_t frame_index) const noexcept
        {
            return frame_index < imgui_descriptors_.size()
                       ? imgui_descriptors_[frame_index]
                       : VK_NULL_HANDLE;
        }

        // ── Overrides ────────────────────────────────────────────────────────

        void resize(VkExtent2D new_extent) override;

        /// Collect both OffscreenImagePool and ImGui texture resources retired at resize/removal.
        void collectRetired(uint64_t frame_id, uint32_t frames_in_flight);

    private:
        void retireImGuiResources();

        struct RetiredImGuiResources
        {
            std::vector<VkDescriptorSet> descriptors;
            VkSampler sampler{VK_NULL_HANDLE};
            uint64_t retire_frame{0};
        };

        std::vector<RetiredImGuiResources> retired_imgui_;

        ImGui_ImplVulkan_Renderer *imgui_renderer_{nullptr};
        std::vector<VkDescriptorSet> imgui_descriptors_;
        VkSampler imgui_sampler_{VK_NULL_HANDLE};
    };

} // namespace lux::ui
