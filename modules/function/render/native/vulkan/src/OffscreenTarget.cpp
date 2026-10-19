#include "ImageFormat.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/OffscreenTarget.hpp>

namespace lux::render::vulkan
{
    OffscreenTarget::OffscreenTarget(DeviceSession& session, TargetDesc description) noexcept
        : session_(session), description_(description)
    {
    }

    RenderResult<std::unique_ptr<OffscreenTarget>> OffscreenTarget::create(
        DeviceSession& session,
        const TargetDesc& description
    ) noexcept
    {
        if (auto valid = description.validate(); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (description.kind != ETargetKind::OFFSCREEN)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto result = std::unique_ptr<OffscreenTarget>(new OffscreenTarget(session, description));
        const auto count = session.framesInFlight();
        result->images_.reserve(count);
        result->views_.reserve(count);
        const auto format = imageFormat(description.format);
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            auto image = session.allocator().createImage(
                {{description.width, description.height},
                 format.native,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 static_cast<VkSampleCountFlagBits>(description.samples)}
            );
            if (!image)
            {
                return cxx::unexpected(image.error());
            }
            result->images_.push_back(std::move(*image));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = result->images_.back().handle();
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format.native;
            view.subresourceRange = {format.aspects, 0, 1, 0, 1};
            VkImageView handle{};
            if (auto created = vkCreateImageView(session.nativeDevice(), &view, nullptr, &handle);
                created != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            result->views_.push_back(handle);
        }
        return result;
    }

    OffscreenTarget::~OffscreenTarget() noexcept
    {
        if (images_.empty())
        {
            return;
        }
        session_.deferRelease(
            [device = session_.nativeDevice(),
             allocator = session_.allocator().native(),
             images = std::move(images_),
             views = std::move(views_)]() mutable noexcept
            {
                for (auto view : views)
                {
                    vkDestroyImageView(device, view, nullptr);
                }
                for (auto& image : images)
                {
                    image.release(allocator);
                }
            }
        );
    }
} // namespace lux::render::vulkan
