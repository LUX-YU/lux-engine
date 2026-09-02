#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui
{
    class UISession;
}

namespace lux::ui::detail
{
    struct UISessionPresentationAccess;

    struct UiFontAtlasSnapshot final
    {
        std::vector<std::uint8_t> pixels;
        int width{};
        int height{};
    };

    class LUX_FUNCTION_PUBLIC UiDrawDataSnapshot final
    {
    public:
        UiDrawDataSnapshot();
        ~UiDrawDataSnapshot();
        UiDrawDataSnapshot(UiDrawDataSnapshot&&) noexcept;
        UiDrawDataSnapshot& operator=(UiDrawDataSnapshot&&) noexcept;
        UiDrawDataSnapshot(const UiDrawDataSnapshot&) = delete;
        UiDrawDataSnapshot& operator=(const UiDrawDataSnapshot&) = delete;

        [[nodiscard]] bool valid() const noexcept;

    private:
        friend struct UISessionPresentationAccess;
        friend class UiVulkanRenderer;
        void captureCurrent();
        [[nodiscard]] const void* nativeDrawData() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

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

    [[nodiscard]] LUX_FUNCTION_PUBLIC UiDrawDataSnapshot captureUiDrawData(UISession& session);
    [[nodiscard]] LUX_FUNCTION_PUBLIC UiFontAtlasSnapshot captureUiFontAtlas(UISession& session);
} // namespace lux::ui::detail
