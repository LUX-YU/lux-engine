#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>

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
        // A sampled Target keeps its last complete publication while another image is being written.
        if (!frame_count || frame_count == UINT32_MAX) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto count = frame_count + std::uint32_t(description.sampled);
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
            auto& progress = result.uses_[slot];
            result.frames_.push_back({image, handle, nullptr, UINT32_MAX, &progress.all, &progress.writes});
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
        result.frame_outputs_.assign(result.frames_.begin(), result.frames_.begin() + frame_count);
        return result;
    }

    OffscreenTarget::OffscreenTarget(OffscreenTarget&& other) noexcept
        : state(other.state), session_(other.session_), description_(other.description_),
          images_(std::move(other.images_)), views_(std::move(other.views_)), frames_(std::move(other.frames_)),
          frame_outputs_(std::move(other.frame_outputs_)), published_image_(other.published_image_),
          sampled_slots_(std::move(other.sampled_slots_)), uses_(std::move(other.uses_))
    {
    }

    OffscreenTarget::SelectedImage OffscreenTarget::selectRead(std::optional<std::uint32_t> writer_image) noexcept
    {
        const auto image = writer_image.value_or(published_image_);
        auto& uses = uses_[image];
        return {
            {.image = frames_[image].image, .queue_uses = {writer_image ? nullptr : &uses.writes, &uses.all}},
            sampled_slots_[image]
        };
    }

    bool OffscreenTarget::writable(std::uint32_t slot) const noexcept
    {
        return session_.reached(uses_[slot].all);
    }

    std::uint32_t OffscreenTarget::writableImage(std::uint32_t preferred) const noexcept
    {
        if (!description_.sampled) return writable(preferred) ? preferred : UINT32_MAX;
        if (preferred != published_image_ && writable(preferred)) return preferred;
        for (std::uint32_t i = 0; i < frames_.size(); ++i)
            if (i != published_image_ && i != preferred && writable(i)) return i;
        return UINT32_MAX;
    }

    void OffscreenTarget::bindOutput(std::uint32_t frame_slot, std::uint32_t image_index) noexcept
    {
        // writableImage selected a retired slot outside the current publication. This Run
        // starts a new producer frontier for that image; readers retain the separate all-use frontier.
        uses_[image_index].writes.clear();
        frame_outputs_[frame_slot] = frames_[image_index];
    }

    void OffscreenTarget::publish(std::uint32_t image) noexcept
    {
        if (!description_.sampled) return;
        published_image_ = image;
    }

    void OffscreenTarget::appendUses(SubmissionFrontiers& uses) const noexcept
    {
        for (const auto& slot : uses_) uses.merge(slot.all);
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
