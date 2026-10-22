#include <algorithm>
#include <bit>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>
#include <lux/engine/render/upload/ImageTransfer.hpp>

namespace lux::render::detail
{
    SampledTexture2DStorage::SampledImage::SampledImage(SampledImage&& other) noexcept
        : image_(std::exchange(other.image_, nullptr)), view_(std::exchange(other.view_, nullptr)),
          descriptor_(std::exchange(other.descriptor_, {})), reference_(other.reference_)
    {
    }

    void SampledTexture2DStorage::releaseImage(
        const nri::CoreInterface& nri_core,
        DescriptorTable& array,
        SampledImage& image
    ) noexcept
    {
        nri_core.DestroyDescriptor(std::exchange(image.view_, nullptr));
        nri_core.DestroyTexture(std::exchange(image.image_, nullptr));
        if (!image.descriptor_.isNull())
        {
            array.release(std::exchange(image.descriptor_, {}));
        }
    }

    namespace
    {
        struct Format final
        {
            nri::Format native;
            std::uint32_t extent, bytes;
            bool srgb;
        };

        Format format(rdesc::ETexturePixelFormat value) noexcept
        {
            switch (value)
            {
            case rdesc::ETexturePixelFormat::RGBA8_UNORM:
                return {nri::Format::RGBA8_UNORM, 1, 4, false};
            case rdesc::ETexturePixelFormat::RGBA8_SRGB:
                return {nri::Format::RGBA8_SRGB, 1, 4, true};
            case rdesc::ETexturePixelFormat::BC1_SRGB:
                return {nri::Format::BC1_RGBA_SRGB, 4, 8, true};
            default:
                return {};
            }
        }
    } // namespace

    RenderResult<SampledTexture2DStorage::ImagePreparation> SampledTexture2DStorage::prepareImage(
        const rdesc::TextureInfo& info,
        std::span<const std::byte> source
    ) noexcept
    {
        auto& session = session_;
        const auto layout = format(info.pixel_format);
        const auto max_dimension = session.description().dimensions.texture2DMaxDim;
        const bool bad_extent = info.width <= 0 || info.height <= 0 || std::uint32_t(info.width) > max_dimension ||
                                std::uint32_t(info.height) > max_dimension;
        const bool bad_count = info.layers != 1 || info.mip_count == 0 || info.mip_count > rdesc::kTextureMaxMipCount;
        const bool bad_color = info.color_space == rdesc::ETextureColorSpace::UNKNOWN ||
                               info.color_space > rdesc::ETextureColorSpace::DATA ||
                               (layout.srgb != (info.color_space == rdesc::ETextureColorSpace::SRGB));
        const bool premultiplied_srgb =
            layout.srgb && rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
        const bool bad_flags =
            (info.flags & ~7u) != 0 ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::COMPRESSED) != (layout.extent == 4)) ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::NO_MIPS) && info.mip_count != 1);
        if (layout.native == nri::Format::UNKNOWN)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (bad_extent || bad_count || bad_color || bad_flags || info.channel != 4)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (layout.extent == 4 && !session.enabledFeatures().texture_compression_bc)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (premultiplied_srgb)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const auto max_mips = std::bit_width(std::max(std::uint32_t(info.width), std::uint32_t(info.height)));
        if (info.mip_count > max_mips)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::uint64_t expected_offset = 0;
        for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
        {
            const auto width = std::max(1u, std::uint32_t(info.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(info.height) >> mip);
            const auto rows = (height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = std::uint64_t((width + layout.extent - 1) / layout.extent) * layout.bytes;
            const auto size = cxx::checkedMul<std::uint64_t>(row, std::uint64_t(rows));
            if (!size)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const auto& input = info.mip_ranges[mip];
            const bool mismatch = input.width != width || input.height != height || input.size != *size ||
                                  input.offset != expected_offset;
            if (mismatch)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto total = cxx::checkedAdd(expected_offset, *size);
            if (!total)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            expected_offset = *total;
        }
        if (expected_offset != source.size() || source.empty())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto support = session.formatSupport(layout.native);
        if ((support & nri::FormatSupportBits::TEXTURE) == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const nri::TextureDesc description{
            nri::TextureType::TEXTURE_2D,
            nri::TextureUsageBits::SHADER_RESOURCE,
            layout.native,
            static_cast<nri::Dim_t>(info.width),
            static_cast<nri::Dim_t>(info.height),
            1,
            static_cast<nri::Dim_t>(info.mip_count),
            1,
            1,
            nri::SharingMode::CONCURRENT
        };
        std::vector<ImageTransferRegion> regions;
        for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
        {
            const auto& input = info.mip_ranges[mip];
            const auto pitch = std::uint64_t((input.width + layout.extent - 1) / layout.extent) * layout.bytes;
            regions.push_back(
                {input.offset,
                 pitch,
                 layout.extent,
                 layout.bytes,
                 {0,
                  0,
                  0,
                  static_cast<nri::Dim_t>(input.width),
                  static_cast<nri::Dim_t>(input.height),
                  1,
                  static_cast<nri::Dim_t>(mip),
                  0,
                  nri::PlaneBits::COLOR}}
            );
        }

        if (auto valid = session.validateTexture(description); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        nri::Texture* image = nullptr;
        const auto created = session.nriCore().CreatePlacedTexture(session.device(), NriDeviceHeap, description, image);
        if (created != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        return ImagePreparation{image, std::move(regions)};
    }

    RenderResult<SampledTexture2DStorage::ImagePreparation> SampledTexture2DStorage::preparePatched(
        const SampledImage& source_image,
        const TextureRegion& region,
        std::span<const std::byte> source
    ) noexcept
    {
        const auto& original = session_.nriCore().GetTextureDesc(*source_image.image_);
        const bool compressed = original.format == nri::Format::BC1_RGBA_SRGB;
        // Check wide Client values before narrowing into NRI's dimension fields.
        const bool invalid = region.mip >= original.mipNum || region.x > original.width || region.y > original.height ||
                             region.width > original.width || region.height > original.height;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const ImageTransferRegion patch{
            0,
            region.row_pitch,
            compressed ? 4u : 1u,
            compressed ? 8u : 4u,
            {static_cast<nri::Dim_t>(region.x),
             static_cast<nri::Dim_t>(region.y),
             0,
             static_cast<nri::Dim_t>(region.width),
             static_cast<nri::Dim_t>(region.height),
             1,
             static_cast<nri::Dim_t>(region.mip),
             0,
             nri::PlaneBits::COLOR}
        };
        const auto size = imageRegionBytes(original, patch, source.size(), UINT64_MAX);
        if (!size || *size != source.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        nri::Texture* image = nullptr;
        const auto created = session_.nriCore().CreatePlacedTexture(session_.device(), NriDeviceHeap, original, image);
        if (created != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        return ImagePreparation{image, {patch}};
    }

    SampledTexture2DStorage::SampledTexture2DStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), frames_(*dependencies.frames), id_(id)
    {
    }

    RenderResult<std::unique_ptr<SampledTexture2DStorage>> SampledTexture2DStorage::create(
        StorageCreateContext& dependencies,
        StorageId id
    ) noexcept
    {
        if (!dependencies.uploads || !dependencies.frames)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto result = std::unique_ptr<SampledTexture2DStorage>(new SampledTexture2DStorage(dependencies, id));
        if (auto ready = result->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        return result;
    }

    RenderResult<void> SampledTexture2DStorage::initialize() noexcept
    {
        if (!session_.descriptors().descriptors())
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        descriptors_ = &session_.descriptors();
        return {};
    }

    SampledTexture2DStorage::~SampledTexture2DStorage() noexcept
    {
        for (auto& image : textures_)
        {
            retire(image);
        }
    }

    const SampledTexture2DStorage::SampledImage* SampledTexture2DStorage::find(TextureId id) const noexcept
    {
        return id.storage == id_ ? textures_.find(id.local) : nullptr;
    }

    RenderResult<SampledTexture2DStorage::SampledImage> SampledTexture2DStorage::sampledImage(
        nri::Texture* image,
        std::uint32_t color_space
    ) noexcept
    {
        SampledImage candidate;
        candidate.image_ = image;
        cxx::scope_exit rollback([&]() noexcept { releaseImage(session_.nriCore(), *descriptors_, candidate); });
        auto descriptor = descriptors_->allocate();
        if (!descriptor)
        {
            return cxx::unexpected(descriptor.error());
        }
        candidate.descriptor_ = *descriptor;
        const auto& description = session_.nriCore().GetTextureDesc(*candidate.image_);
        const nri::TextureViewDesc
            view{candidate.image_, nri::TextureView::TEXTURE, description.format, 0, description.mipNum, 0, 1};
        auto status = session_.nriCore().CreateTextureView(view, candidate.view_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::SamplerDesc sampler{};
        sampler.filters = {nri::Filter::NEAREST, nri::Filter::NEAREST, nri::Filter::NEAREST};
        sampler.addressModes =
            {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};
        sampler.mipMax = float(description.mipNum - 1);
        auto sampler_index = descriptors_->sampler(sampler);
        if (!sampler_index)
        {
            return cxx::unexpected(sampler_index.error());
        }
        candidate.reference_ = {descriptor->index, *sampler_index, color_space, 0};
        descriptors_->write(candidate.descriptor_, candidate.view_);
        rollback.release();
        return candidate;
    }

    void SampledTexture2DStorage::retire(SampledImage& value) noexcept
    {
        if (!value.image_)
        {
            return;
        }
        session_.deferRelease([value = std::move(value), nri_core = &session_.nriCore(), descriptors = descriptors_](
                              ) mutable noexcept { releaseImage(*nri_core, *descriptors, value); });
    }

    SampledTexture2DStorage::UploadResult SampledTexture2DStorage::upload(TextureWrite input) noexcept
    {
        if (input.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto prepared = prepareImage(input.info, input.source);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = sampledImage(std::exchange(prepared->first, nullptr), std::uint32_t(input.info.color_space));
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        cxx::scope_exit rollback([&]() noexcept { retire(*candidate); });
        auto accepted = co_await uploads_.writeImage(*candidate->image_, input.source, std::move(prepared->second));
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        co_return TextureId{id_, textures_.emplace(std::move(*candidate))};
    }

    SampledTexture2DStorage::UploadResult SampledTexture2DStorage::upload(TextureRegionWrite input) noexcept
    {
        const auto* original = find(input.texture);
        if (!original)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        // The Client retains this immutable source through Response. No shared physical owner is needed.
        auto prepared = preparePatched(*original, input.region, input.source);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = sampledImage(std::exchange(prepared->first, nullptr), original->reference_.color_space);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        cxx::scope_exit rollback([&]() noexcept { retire(*candidate); });
        auto accepted =
            co_await uploads_
                .writeImage(*candidate->image_, input.source, std::move(prepared->second), original->image_);
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        co_return TextureId{id_, textures_.emplace(std::move(*candidate))};
    }

    RenderResult<void> SampledTexture2DStorage::release(TextureId id) noexcept
    {
        if (frames_.submitting())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        auto* value = id.storage == id_ ? textures_.find(id.local) : nullptr;
        if (!value)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        retire(*value);
        textures_.erase(id.local);
        return {};
    }

    RenderResult<ResourceInput> SampledTexture2DStorage::
        resolve(const VGraphResourceDesc& plan, const TextureBinding& argument, StorageSubscription&) noexcept
    {
        const auto* backing = find(argument.texture);
        if (backing)
        {
            const auto* expected = std::get_if<GraphImageDesc>(&plan);
            if (!expected)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            const auto native_format = expected->format == rdesc::ETextureFormat::RGBA8_UNORM ? nri::Format::RGBA8_UNORM
                                       : expected->format == rdesc::ETextureFormat::RGBA8_SRGB
                                           ? nri::Format::RGBA8_SRGB
                                           : nri::Format::BC1_RGBA_SRGB;
            const auto& image = session_.nriCore().GetTextureDesc(*backing->image_);
            const bool is_mismatch = expected->width != image.width || expected->height != image.height ||
                                     expected->mips != image.mipNum || expected->layers != image.layerNum ||
                                     native_format != image.format;
            if (is_mismatch)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        static const rdesc::ShaderBlock layout = []
        {
            rdesc::ShaderBlock result{sizeof(TextureGpuRef), alignof(TextureGpuRef)};
            result.fields = {
                {"image_index", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, image_index), 1, 1, 0, 0},
                {"sampler_index", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, sampler_index), 1, 1, 0, 0},
                {"color_space", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, color_space), 1, 1, 0, 0},
                {"flags", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, flags), 1, 1, 0, 0}
            };
            return result;
        }();
        // Texture identities are immutable. The Client ends bindings before release; there is no update subscription.
        static constexpr TextureGpuRef default_reference{};
        const auto& reference = backing ? backing->reference_ : default_reference;
        return ResourceInput{nullptr, 0, 0, std::as_bytes(std::span(&reference, 1)), &layout, 1};
    }

    StorageFactory SampledTexture2DStorage::factory() noexcept
    {
        static const StorageBindingOps bindings{
            [](void* storage,
               const VGraphResourceDesc& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept -> RenderResult<void>
            {
                auto value = static_cast<SampledTexture2DStorage*>(storage)
                                 ->resolve(plan, *static_cast<const TextureBinding*>(argument), subscription);
                if (!value)
                {
                    return cxx::unexpected(value.error());
                }
                std::ranges::fill(output, *value);
                return {};
            }
        };
        static const auto factory = makeStorageFactory<
            render::SampledTexture2DStorageDescriptor,
            SampledTexture2DStorage,
            StorageCreateContext,
            &SampledTexture2DStorage::create>(nullptr, &bindings);
        return factory;
    }
} // namespace lux::render::detail

namespace lux::render
{
    StorageTraits<detail::SampledTexture2DStorage>::Result StorageTraits<detail::SampledTexture2DStorage>::upload(
        detail::SampledTexture2DStorage& storage,
        TextureWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    StorageTraits<detail::SampledTexture2DStorage>::Result StorageTraits<detail::SampledTexture2DStorage>::upload(
        detail::SampledTexture2DStorage& storage,
        TextureRegionWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
