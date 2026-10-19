#pragma once

#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <vector>

namespace lux::render::vulkan
{
    class DeviceSession;

    struct TargetFrame final
    {
        gapi::vk::VmaImage* image = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    // One actual output owner. Contexts only borrow the matching frame slice.
    class OffscreenTarget final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<OffscreenTarget>>
        create(DeviceSession&, const TargetDesc&) noexcept;
        ~OffscreenTarget() noexcept;
        OffscreenTarget(const OffscreenTarget&) = delete;
        OffscreenTarget& operator=(const OffscreenTarget&) = delete;

        [[nodiscard]] const TargetDesc& description() const noexcept
        {
            return description_;
        }

        [[nodiscard]] DeviceSession& session() const noexcept
        {
            return session_;
        }

        [[nodiscard]] TargetFrame frame(std::uint32_t slot) noexcept
        {
            return {&images_[slot], views_[slot]};
        }

    private:
        OffscreenTarget(DeviceSession&, TargetDesc) noexcept;
        DeviceSession& session_;
        TargetDesc description_;
        std::vector<gapi::vk::VmaImage> images_;
        std::vector<VkImageView> views_;
    };
} // namespace lux::render::vulkan
