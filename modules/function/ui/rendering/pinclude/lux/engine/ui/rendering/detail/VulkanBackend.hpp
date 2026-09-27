#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/FontAtlas.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui::detail
{
    enum class EVulkanBackendError : std::uint8_t
    {
        INVALID_CONFIG,
        RENDERER_CREATE_FAILURE,
        FONT_UPLOAD_FAILURE,
        BUFFER_CREATE_FAILURE,
    };

    struct VulkanRendererCreateInfo final
    {
        VkInstance instance{};
        VkPhysicalDevice physical_device{};
        VkDevice device{};
        std::uint32_t queue_family{};
        VkQueue queue{};
        VkFormat color_format{VK_FORMAT_UNDEFINED};
        std::uint32_t image_count{};
        const VkAllocationCallbacks* allocator{};
    };

    class VulkanRenderer final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<VulkanRenderer>, EVulkanBackendError>;

        [[nodiscard]] static CreateResult create(const VulkanRendererCreateInfo& info, const FontAtlas& font) noexcept;

        ~VulkanRenderer();
        VulkanRenderer(const VulkanRenderer&) = delete;
        VulkanRenderer& operator=(const VulkanRenderer&) = delete;

        void render(const DrawData* snapshot, VkCommandBuffer command) noexcept;
        void render(const DrawData* snapshot, VkCommandBuffer command, VkPipeline pipeline) noexcept;
        // The caller has waited this GPU FIF slot. All Views in the same serial
        // use the same immutable snapshot and upload its geometry exactly once.
        void renderFrame(
            const DrawData& snapshot,
            VkCommandBuffer command,
            VkPipeline pipeline,
            std::uint32_t frame_slot,
            std::uint64_t serial
        ) noexcept;
        [[nodiscard]] VkPipeline createColorPipeline(VkFormat format);
        void destroyColorPipeline(VkPipeline pipeline) noexcept;
        using TextureResolver = VkDescriptorSet (*)(void* user, render::RTextureHandle texture) noexcept;
        // Render-thread only. The provider owns descriptors through GPU completion.
        void setTextureResolver(TextureResolver resolver, void* user) noexcept;
        [[nodiscard]] VkDescriptorSet addTexture(VkSampler sampler, VkImageView view, VkImageLayout layout);
        void removeTexture(VkDescriptorSet descriptor) noexcept;

    private:
        struct Impl;
        explicit VulkanRenderer(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ui::detail
