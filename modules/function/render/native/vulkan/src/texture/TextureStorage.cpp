#include "Shader.hpp"
#include <algorithm>
#include <bit>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    TextureStorage::SampledImage::SampledImage(SampledImage&& other) noexcept
        : image_(std::move(other.image_)), view_(std::exchange(other.view_, VK_NULL_HANDLE)),
          sampler_(std::move(other.sampler_)), descriptor_(std::exchange(other.descriptor_, {})),
          reference_(other.reference_)
    {
    }

    void TextureStorage::SampledImage::release(VkDevice device, VmaAllocator allocator, DescriptorArray& array) noexcept
    {
        if (view_)
        {
            vkDestroyImageView(device, std::exchange(view_, VK_NULL_HANDLE), nullptr);
        }
        sampler_.release(device);
        image_.release(allocator);
        if (!descriptor_.isNull())
        {
            array.release(std::exchange(descriptor_, {}));
        }
    }

    namespace
    {
        struct Format final
        {
            VkFormat native;
            std::uint32_t extent, bytes;
            bool srgb;
        };

        Format format(rdesc::ETexturePixelFormat value) noexcept
        {
            switch (value)
            {
            case rdesc::ETexturePixelFormat::RGBA8_UNORM:
                return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false};
            case rdesc::ETexturePixelFormat::RGBA8_SRGB:
                return {VK_FORMAT_R8G8B8A8_SRGB, 1, 4, true};
            case rdesc::ETexturePixelFormat::BC1_SRGB:
                return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8, true};
            default:
                return {};
            }
        }
    } // namespace

    RenderResult<TextureStorage::ImagePreparation> TextureStorage::prepareImage(
        const rdesc::TextureInfo& info,
        std::span<const std::byte> source
    ) noexcept
    {
        auto& session = session_;
        const auto layout = format(info.pixel_format);
        const auto max_dimension = session.physicalDevice().properties().properties.limits.maxImageDimension2D;
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
        if (layout.native == VK_FORMAT_UNDEFINED)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        if (bad_extent || bad_count || bad_color || bad_flags || info.channel != 4)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (layout.extent == 4 && !session.enabledFeatures().texture_compression_bc)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        if (premultiplied_srgb)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const auto max_mips = std::bit_width(std::max(std::uint32_t(info.width), std::uint32_t(info.height)));
        if (info.mip_count > max_mips)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
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
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& input = info.mip_ranges[mip];
            const bool mismatch = input.width != width || input.height != height || input.size != *size ||
                                  input.offset != expected_offset;
            if (mismatch)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto total = cxx::checkedAdd(expected_offset, *size);
            if (!total)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            expected_offset = *total;
        }
        if (expected_offset != source.size() || source.empty())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto properties = session.physicalDevice().formatProperties(layout.native);
        constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const ImageDescription description{
            {std::uint32_t(info.width), std::uint32_t(info.height)},
            layout.native,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            info.mip_count,
            1
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
                 {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
                 {},
                 {input.width, input.height, 1}}
            );
        }

        auto image = session.allocator().createImage(description);
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        return ImagePreparation{std::move(*image), std::move(regions)};
    }

    RenderResult<TextureStorage::ImagePreparation> TextureStorage::preparePatched(
        const SampledImage& source_image,
        const TextureRegion& region,
        std::span<const std::byte> source
    ) noexcept
    {
        const auto& original = source_image.image_;
        if (region.mip >= original.mipLevels())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto width = std::max(1u, original.extent().width >> region.mip);
        const auto height = std::max(1u, original.extent().height >> region.mip);
        const bool compressed = original.format() == VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const std::uint32_t block_extent = compressed ? 4 : 1;
        const std::uint32_t block_bytes = compressed ? 8 : 4;
        const bool invalid_offset = region.x >= width || region.y >= height;
        if (invalid_offset)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const bool invalid_range =
            !region.width || !region.height || region.width > width - region.x || region.height > height - region.y;
        const bool invalid_blocks = region.x % block_extent || region.y % block_extent ||
                                    (region.width % block_extent && region.width != width - region.x) ||
                                    (region.height % block_extent && region.height != height - region.y);
        if (invalid_range || invalid_blocks)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const std::uint64_t row_bytes = std::uint64_t((region.width - 1) / block_extent + 1) * block_bytes;
        const auto rows = (region.height - 1) / block_extent + 1;
        if (region.row_pitch < row_bytes || region.row_pitch % block_bytes)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto stride = cxx::checkedMul<std::uint64_t>(rows - 1, region.row_pitch);
        if (!stride)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto total = cxx::checkedAdd(*stride, row_bytes);
        if (!total || *total != source.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto image = session_.allocator().createImage(
            {{original.extent().width, original.extent().height},
             original.format(),
             original.usage(),
             original.samples(),
             original.mipLevels(),
             original.arrayLayers()}
        );
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        std::vector<ImageTransferRegion> regions{
            {0,
             region.row_pitch,
             block_extent,
             block_bytes,
             {VK_IMAGE_ASPECT_COLOR_BIT, region.mip, 0, 1},
             {std::int32_t(region.x), std::int32_t(region.y), 0},
             {region.width, region.height, 1}}
        };
        return ImagePreparation{std::move(*image), std::move(regions)};
    }

    TextureStorage::TextureStorage(DeviceSession& session, StorageId id) noexcept : session_(session), id_(id) {}

    RenderResult<std::unique_ptr<TextureStorage>> TextureStorage::create(DeviceSession& session, StorageId id) noexcept
    {
        auto result = std::unique_ptr<TextureStorage>(new TextureStorage(session, id));
        if (auto ready = result->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        return result;
    }

    RenderResult<void> TextureStorage::initialize() noexcept
    {
        auto table = session_.sampledImageArray();
        if (!table)
        {
            return cxx::unexpected(table.error());
        }
        descriptors_ = *table;
        return {};
    }

    TextureStorage::~TextureStorage() noexcept
    {
        for (auto& image : textures_)
        {
            retire(image);
        }
    }

    const TextureStorage::SampledImage* TextureStorage::find(TextureId id) const noexcept
    {
        return id.storage == id_ ? textures_.find(id.local) : nullptr;
    }

    RenderResult<TextureStorage::SampledImage> TextureStorage::sampledImage(
        gapi::vk::VmaImage image,
        std::uint32_t color_space
    ) noexcept
    {
        SampledImage candidate;
        candidate.image_ = std::move(image);
        cxx::scope_exit rollback(
            [&]() noexcept { candidate.release(session_.nativeDevice(), session_.allocator().native(), *descriptors_); }
        );
        auto descriptor = descriptors_->allocate();
        if (!descriptor)
        {
            return cxx::unexpected(descriptor.error());
        }
        candidate.descriptor_ = *descriptor;
        candidate.reference_ = {descriptor->index, color_space};
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = candidate.image_.handle();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = candidate.image_.format();
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, candidate.image_.mipLevels(), 0, 1};
        auto status = vkCreateImageView(session_.nativeDevice(), &view, nullptr, &candidate.view_);
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = float(candidate.image_.mipLevels() - 1);
        status = candidate.sampler_.tryCreate(session_.nativeDevice(), sampler);
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        descriptors_->write(candidate.descriptor_, candidate.view_, candidate.sampler_);
        rollback.release();
        return candidate;
    }

    void TextureStorage::retire(SampledImage& value) noexcept
    {
        if (!value.image_)
        {
            return;
        }
        session_.deferRelease([value = std::move(value),
                               device = session_.nativeDevice(),
                               allocator = session_.allocator().native(),
                               descriptors = descriptors_]() mutable noexcept
                              { value.release(device, allocator, *descriptors); });
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureWrite input) noexcept
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
        auto candidate = sampledImage(std::move(prepared->first), std::uint32_t(input.info.color_space));
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        cxx::scope_exit rollback([&]() noexcept { retire(*candidate); });
        auto accepted =
            co_await session_.uploads().writeImage(candidate->image_, input.source, std::move(prepared->second));
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kWorkStopped});
        }
        co_return TextureId{id_, textures_.emplace(std::move(*candidate))};
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureRegionWrite input) noexcept
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
        auto candidate = sampledImage(std::move(prepared->first), original->reference_.color_space);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        cxx::scope_exit rollback([&]() noexcept { retire(*candidate); });
        auto accepted =
            co_await session_.uploads()
                .writeImage(candidate->image_, input.source, std::move(prepared->second), &original->image_);
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kWorkStopped});
        }
        co_return TextureId{id_, textures_.emplace(std::move(*candidate))};
    }

    RenderResult<void> TextureStorage::release(TextureId id) noexcept
    {
        auto* value = id.storage == id_ ? textures_.find(id.local) : nullptr;
        if (!value)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        retire(*value);
        textures_.erase(id.local);
        return {};
    }

    RenderResult<StorageResource> TextureStorage::
        resolve(const NativeResourcePlan& plan, const TextureBinding& argument, StorageSubscription&) noexcept
    {
        const auto* backing = find(argument.texture);
        if (!backing)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* expected = std::get_if<GraphImageDesc>(&plan.description);
        if (!expected)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto native_format = expected->format == rdesc::ETextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                                   : expected->format == rdesc::ETextureFormat::RGBA8_SRGB
                                       ? VK_FORMAT_R8G8B8A8_SRGB
                                       : VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const auto& image = backing->image_;
        const bool is_mismatch = expected->width != image.extent().width || expected->height != image.extent().height ||
                                 expected->mips != image.mipLevels() || expected->layers != image.arrayLayers() ||
                                 native_format != image.format();
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        static const rdesc::ShaderBlock layout = []
        {
            rdesc::ShaderBlock result{sizeof(TextureGpuRef), alignof(TextureGpuRef)};
            result.fields = {
                {"descriptor_index", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, descriptor_index), 1, 1, 0, 0},
                {"color_space", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, color_space), 1, 1, 0, 0}
            };
            return result;
        }();
        // Texture identities are immutable. The Client ends bindings before release; there is no update subscription.
        return StorageResource{VK_NULL_HANDLE, 0, 0, std::as_bytes(std::span(&backing->reference_, 1)), &layout};
    }

    StorageFactory textureStorageFactory() noexcept
    {
        static const StorageBindingOps bindings{
            [](void* storage, const NativeResourcePlan& plan, const void* argument, StorageSubscription& subscription
            ) noexcept
            {
                return static_cast<TextureStorage*>(storage)
                    ->resolve(plan, *static_cast<const TextureBinding*>(argument), subscription);
            }
        };
        static const auto factory =
            makeStorageFactory<render::TextureStorage, TextureStorage, DeviceSession, &TextureStorage::create>(
                nullptr,
                &bindings
            );
        return factory;
    }
} // namespace lux::render::vulkan

namespace lux::render
{
    StorageTraits<vulkan::TextureStorage>::Result StorageTraits<vulkan::TextureStorage>::upload(
        vulkan::TextureStorage& storage,
        TextureWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    StorageTraits<vulkan::TextureStorage>::Result StorageTraits<vulkan::TextureStorage>::upload(
        vulkan::TextureStorage& storage,
        TextureRegionWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
