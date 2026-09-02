#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>

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
        const VkAllocationCallbacks* allocator{};
    };

    class UiVulkanRenderer final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<UiVulkanRenderer>, EUiVulkanBackendError>;

        [[nodiscard]] static CreateResult create(
            const UiVulkanRendererCreateInfo& info,
            const UiFontAtlasSnapshot& font
        ) noexcept;

        ~UiVulkanRenderer();
        UiVulkanRenderer(const UiVulkanRenderer&) = delete;
        UiVulkanRenderer& operator=(const UiVulkanRenderer&) = delete;

        void render(const UiDrawDataSnapshot* snapshot, VkCommandBuffer command) noexcept;

    private:
        struct Impl;
        explicit UiVulkanRenderer(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ui::detail
