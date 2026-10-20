#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>

namespace lux::render::detail
{
    OffscreenTarget::OffscreenTarget(DeviceSession& session, TargetDesc description) noexcept
        : session_(session), description_(description)
    {
    }

    RenderResult<std::unique_ptr<OffscreenTarget>> OffscreenTarget::create(
        DeviceSession& session,
        const TargetDesc& description,
        std::uint32_t frame_count
    ) noexcept
    {
        if (auto valid = description.validate(); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (description.kind != ETargetKind::OFFSCREEN)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const auto maximum = session.description().dimensions.attachmentMaxDim;
        if (description.width > maximum || description.height > maximum)
        {
            return cxx::unexpected(RenderError{kInvalidTargetExtent});
        }
        auto result = std::unique_ptr<OffscreenTarget>(new OffscreenTarget(session, description));
        const auto count = frame_count;
        result->images_.reserve(count);
        result->views_.reserve(count);
        const auto format = imageFormat(description.format);
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            const nri::TextureDesc image_description{
                nri::TextureType::TEXTURE_2D,
                nri::TextureUsageBits::COLOR_ATTACHMENT,
                format.native,
                static_cast<nri::Dim_t>(description.width),
                static_cast<nri::Dim_t>(description.height),
                1,
                1,
                1,
                static_cast<nri::Sample_t>(description.samples),
                nri::SharingMode::EXCLUSIVE
            };
            if (slot == 0)
            {
                if (auto valid = session.validateTexture(image_description); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
            }
            nri::Texture* image = nullptr;
            const auto created =
                session.nriCore().CreatePlacedTexture(session.device(), NriDeviceHeap, image_description, image);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            result->images_.push_back(image);
            const nri::TextureViewDesc view{image, nri::TextureView::COLOR_ATTACHMENT, format.native, 0, 1, 0, 1};
            nri::Descriptor* handle = nullptr;
            if (const auto created = session.nriCore().CreateTextureView(view, handle); created != nri::Result::SUCCESS)
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
            [api = &session_.nriCore(), images = std::move(images_), views = std::move(views_)]() mutable noexcept
            {
                for (auto view : views)
                {
                    api->DestroyDescriptor(view);
                }
                for (auto& image : images)
                {
                    api->DestroyTexture(image);
                }
            }
        );
    }
} // namespace lux::render::detail
