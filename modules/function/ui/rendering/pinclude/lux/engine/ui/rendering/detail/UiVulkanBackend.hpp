#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ui/TextureHandle.hpp>
#include <lux/engine/ui/UiFrameSnapshot.hpp>
#include <lux/engine/ui/detail/UiFontAtlas.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui::detail
{
enum class EUiVulkanBackendError : std::uint8_t
{
    INVALID_CONFIG,
    RENDERER_CREATE_FAILURE,
    FONT_UPLOAD_FAILURE,
    BUFFER_CREATE_FAILURE,
};

struct UiVulkanRendererCreateInfo final
{
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t queue_family{};
    VkQueue queue{};
    VkFormat color_format{VK_FORMAT_UNDEFINED};
    std::uint32_t image_count{};
    const VkAllocationCallbacks *allocator{};
};

class UiVulkanRenderer final
{
  public:
    using CreateResult = lux::cxx::expected<std::unique_ptr<UiVulkanRenderer>, EUiVulkanBackendError>;

    [[nodiscard]] static CreateResult create(const UiVulkanRendererCreateInfo &info,
                                             const UiFontAtlasSnapshot &font) noexcept;

    ~UiVulkanRenderer();
    UiVulkanRenderer(const UiVulkanRenderer &) = delete;
    UiVulkanRenderer &operator=(const UiVulkanRenderer &) = delete;

    void render(const UiFrameSnapshot *snapshot, VkCommandBuffer command) noexcept;
    void render(const UiFrameSnapshot *snapshot, VkCommandBuffer command, VkPipeline pipeline) noexcept;
    // The caller has waited this GPU FIF slot. All Views in the same serial
    // use the same immutable snapshot and upload its geometry exactly once.
    void renderFrame(const UiFrameSnapshot &snapshot, VkCommandBuffer command, VkPipeline pipeline,
                     std::uint32_t frame_slot, std::uint64_t serial) noexcept;
    [[nodiscard]] VkPipeline createColorPipeline(VkFormat format);
    void destroyColorPipeline(VkPipeline pipeline) noexcept;
    using TextureResolver = VkDescriptorSet (*)(void *user, TextureHandle texture) noexcept;
    // Render-thread only. The provider owns descriptors through GPU completion.
    void setTextureResolver(TextureResolver resolver, void *user) noexcept;
    [[nodiscard]] VkDescriptorSet addTexture(VkSampler sampler, VkImageView view, VkImageLayout layout);
    void removeTexture(VkDescriptorSet descriptor) noexcept;

  private:
    struct Impl;
    explicit UiVulkanRenderer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace lux::ui::detail
