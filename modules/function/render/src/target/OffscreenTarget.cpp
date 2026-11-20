#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

namespace lux::render::detail
{
    OffscreenTarget::OffscreenTarget(DeviceSession& session, TargetDesc description) noexcept
        : session_(session), description_(description)
    {
    }

    RenderResult<OffscreenTarget>
    OffscreenTarget::create(DeviceSession& session, const TargetDesc& description, std::uint32_t frame_count) noexcept
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
        OffscreenTarget result(session, description);
        const auto count = frame_count;
        result.uses_.assign(count, ImageProgress{session.makeFrontiers(), session.makeFrontiers()});
        result.images_.reserve(count);
        result.views_.reserve(count);
        result.frames_.reserve(count);
        const auto format = imageFormat(description.format);
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            const nri::TextureDesc image_description{
                nri::TextureType::TEXTURE_2D,
                description.sampled ? nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE
                                    : nri::TextureUsageBits::COLOR_ATTACHMENT,
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
            result.images_.push_back(image);
            const nri::TextureViewDesc view{image, nri::TextureView::COLOR_ATTACHMENT, format.native, 0, 1, 0, 1};
            nri::Descriptor* handle = nullptr;
            if (const auto created = session.nriCore().CreateTextureView(view, handle); created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            result.views_.push_back(handle);
            result.frames_.push_back({image, handle, nullptr, UINT32_MAX, &result.uses_[slot].all});
            if (description.sampled)
            {
                auto allocated = session.descriptors().allocate(nri::DescriptorType::TEXTURE);
                if (!allocated)
                {
                    return cxx::unexpected(allocated.error());
                }
                result.sampled_slots_.push_back(*allocated);
                nri::Descriptor* sampled = nullptr;
                const nri::TextureViewDesc sampled_view{image, nri::TextureView::TEXTURE, format.native};
                const auto created = session.nriCore().CreateTextureView(sampled_view, sampled);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
                result.views_.push_back(sampled);
                session.descriptors().write(nri::DescriptorType::TEXTURE, *allocated, sampled);
                result.frames_.back().sampled_index = allocated->index;
            }
        }
        return result;
    }

    OffscreenTarget::OffscreenTarget(OffscreenTarget&& other) noexcept
        : state(other.state), session_(other.session_), description_(other.description_),
          images_(std::move(other.images_)), views_(std::move(other.views_)), frames_(std::move(other.frames_)),
          sampled_slots_(std::move(other.sampled_slots_)), uses_(std::move(other.uses_)),
          content_revision_(other.content_revision_)
    {
    }

    bool OffscreenTarget::readable(std::uint32_t slot) const noexcept
    {
        return session_.reached(uses_[slot].writes);
    }

    bool OffscreenTarget::writable(std::uint32_t slot) const noexcept
    {
        return session_.reached(uses_[slot].all);
    }

    void OffscreenTarget::acceptRead(std::uint32_t slot, const SubmissionFrontiers& uses) noexcept
    {
        uses_[slot].all.merge(uses);
    }

    void OffscreenTarget::acceptWrite(std::uint32_t slot, const SubmissionFrontiers& uses) noexcept
    {
        uses_[slot].writes.merge(uses);
        uses_[slot].content = 0;
    }

    void OffscreenTarget::appendUses(SubmissionFrontiers& uses) const noexcept
    {
        for (const auto& slot : uses_) uses.merge(slot.all);
    }

    std::uint64_t OffscreenTarget::publish(std::uint32_t slot) noexcept
    {
        if (content_revision_ == UINT64_MAX) std::terminate();
        return uses_[slot].content = ++content_revision_;
    }

    OffscreenTarget::~OffscreenTarget() noexcept
    {
        if (images_.empty())
        {
            return;
        }
        auto accepted = session_.makeFrontiers();
        appendUses(accepted);
        session_.deferRelease(
            accepted,
            [nri_core = &session_.nriCore(),
             descriptors = &session_.descriptors(),
             slots = std::move(sampled_slots_),
             images = std::move(images_),
             views = std::move(views_)]() mutable noexcept
            {
                for (auto slot : slots)
                {
                    descriptors->release(nri::DescriptorType::TEXTURE, slot);
                }
                for (auto view : views)
                {
                    nri_core->DestroyDescriptor(view);
                }
                for (auto& image : images)
                {
                    nri_core->DestroyTexture(image);
                }
            }
        );
    }
} // namespace lux::render::detail
