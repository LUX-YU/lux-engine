#include "Shader.hpp"
#include <algorithm>
#include <bit>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    TextureStorage::SampledImage::SampledImage(
        gapi::vk::VmaImage image,
        DescriptorArray::Slot descriptor,
        const rdesc::TextureInfo& info,
        std::uint64_t revision
    ) noexcept
        : image_(std::move(image)), descriptor_(std::move(descriptor)), info_(info), revision_(revision),
          reference_{descriptor_.index, static_cast<std::uint32_t>(info.color_space)}
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
        array.release(std::exchange(descriptor_, {}));
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
        std::span<const std::byte> source,
        std::optional<TextureRegion> region
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
        if (region)
        {
            if (region->mip >= info.mip_count)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& mip = info.mip_ranges[region->mip];
            const bool invalid_offset = region->x >= mip.width || region->y >= mip.height;
            if (invalid_offset)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const bool invalid_range = !region->width || !region->height || region->width > mip.width - region->x ||
                                       region->height > mip.height - region->y;
            const bool invalid_blocks = region->x % layout.extent || region->y % layout.extent ||
                                        (region->width % layout.extent && region->width != mip.width - region->x) ||
                                        (region->height % layout.extent && region->height != mip.height - region->y);
            if (invalid_range || invalid_blocks)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto rows = (region->height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = ((region->width + layout.extent - 1) / layout.extent) * layout.bytes;
            const bool invalid_pitch = region->row_pitch < row || region->row_pitch % layout.bytes;
            if (invalid_pitch)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto stride = cxx::checkedMul<std::uint64_t>(rows - 1, region->row_pitch);
            if (!stride)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto total = cxx::checkedAdd(*stride, row);
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
        if (region)
        {
            regions.push_back(
                {0,
                 region->row_pitch,
                 layout.extent,
                 layout.bytes,
                 {VK_IMAGE_ASPECT_COLOR_BIT, region->mip, 0, 1},
                 {std::int32_t(region->x), std::int32_t(region->y), 0},
                 {region->width, region->height, 1}}
            );
        }
        else
        {
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
        }
        auto image = session.allocator().createImage(description);
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        return ImagePreparation{std::move(*image), std::move(regions)};
    }

    // Exclusive reservation of an existing Storage candidate. No transfer, queue or async phase is owned here.
    struct TextureStorage::Candidate final
    {
        TextureStorage& storage;
        std::unique_ptr<SampledImage> version;
        TextureId texture;
        const SampledImage* base = nullptr;

        Candidate(TextureStorage& owner, std::unique_ptr<SampledImage> value, TextureId id) noexcept
            : storage(owner), version(std::move(value)), texture(id)
        {
        }

        Candidate(Candidate&&) noexcept = default;
        Candidate(const Candidate&) = delete;

        ~Candidate() noexcept
        {
            if (version)
            {
                storage.rollback(*this);
            }
        }
    };

    TextureStorage::Observer::~Observer() noexcept
    {
        reset();
    }

    void TextureStorage::Observer::reset() noexcept
    {
        if (source_)
        {
            auto& records = source_->observers_;
            records[position_] = records.back();
            records[position_]->position_ = position_;
            records.pop_back();
            source_ = nullptr;
        }
    }

    void TextureStorage::observe(Observer& observer) noexcept
    {
        observer.reset();
        observer.source_ = this;
        observer.position_ = observers_.size();
        observers_.push_back(&observer);
    }

    StorageResource TextureStorage::resource(const SampledImage* value) noexcept
    {
        static const TextureGpuRef white{};
        static const rdesc::ShaderBlock layout = []
        {
            rdesc::ShaderBlock result{sizeof(TextureGpuRef), alignof(TextureGpuRef)};
            result.fields = {
                {"descriptor_index", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, descriptor_index), 1, 1, 0, 0},
                {"color_space", rdesc::EDataScalar::UINT, offsetof(TextureGpuRef, color_space), 1, 1, 0, 0}
            };
            return result;
        }();
        return {nullptr, 0, 0, std::as_bytes(std::span(value ? &value->reference_ : &white, 1)), &layout};
    }

    void TextureStorage::disconnect(TextureRecord& record) noexcept
    {
        StorageSubscription::disconnect(record.bindings, resource(nullptr));
    }

    RenderResult<StorageResource> TextureStorage::resolve(
        const NativeResourcePlan& plan,
        const TextureBinding& argument,
        StorageSubscription& subscription
    ) noexcept
    {
        auto* record = argument.texture.storage == id_ ? textures_.find(argument.texture.local) : nullptr;
        if (!record || !record->current)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& backing = *record->current;
        const auto* expected = std::get_if<GraphImageDesc>(&plan.description);
        if (!expected)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto& actual = backing.info();
        const auto native_format = expected->format == rdesc::ETextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                                   : expected->format == rdesc::ETextureFormat::RGBA8_SRGB
                                       ? VK_FORMAT_R8G8B8A8_SRGB
                                       : VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const bool is_mismatch = expected->width != actual.width || expected->height != actual.height ||
                                 expected->mips != actual.mip_count || expected->layers != actual.layers ||
                                 native_format != backing.image().format();
        if (is_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(record->bindings);
        return resource(&backing);
    }

    TextureStorage::TextureStorage(DeviceSession& session, StorageId id) noexcept : session_(session), id_(id) {}

    StorageFactory textureStorageFactory() noexcept
    {
        static const lux::render::TextureStorage::Ops ops{
            [](const void* value, TextureId id) noexcept -> RenderResult<TexturePublished>
            {
                auto* record = static_cast<const vulkan::TextureStorage*>(value)->find(id);
                if (!record)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                return TexturePublished{id, record->revision()};
            }
        };
        static const StorageBindingOps binding_ops{
            [](void* storage, const NativeResourcePlan& plan, const void* argument, StorageSubscription& subscription
            ) noexcept
            {
                return static_cast<TextureStorage*>(storage)
                    ->resolve(plan, *static_cast<const TextureBinding*>(argument), subscription);
            }
        };
        static const auto factory = makeStorageFactory<
            lux::render::TextureStorage,
            vulkan::TextureStorage,
            DeviceSession,
            &vulkan::TextureStorage::create>(ops, &binding_ops);
        return factory;
    }

    RenderResult<std::unique_ptr<TextureStorage>> TextureStorage::create(DeviceSession& session, StorageId id) noexcept
    {
        if (!session.enabledFeatures().texture_descriptors)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto storage = std::unique_ptr<TextureStorage>(new TextureStorage(session, id));
        if (auto result = storage->initialize(); !result)
        {
            return cxx::unexpected(result.error());
        }
        return storage;
    }

    RenderResult<void> TextureStorage::initialize() noexcept
    {
        auto table = session_.sampledImageArray();
        if (!table)
        {
            return cxx::unexpected(table.error());
        }
        descriptors_ = std::move(*table);
        return {};
    }

    TextureStorage::~TextureStorage() noexcept
    {
        for (auto* observer : observers_)
        {
            observer->source_ = nullptr;
        }
        for (auto& record : textures_)
        {
            disconnect(record);
            retire(record.current);
        }
    }

    const TextureStorage::SampledImage* TextureStorage::find(TextureId id) const noexcept
    {
        if (id.storage != id_)
        {
            return nullptr;
        }
        const auto* record = textures_.find(id.local);
        return record ? record->current.get() : nullptr;
    }

    RenderResult<TextureStorage::Candidate> TextureStorage::reserve(
        gapi::vk::VmaImage image,
        const rdesc::TextureInfo& info,
        TextureId id,
        std::uint64_t expected
    ) noexcept
    {
        cxx::scope_exit release_image([&]() noexcept { image.release(session_.allocator().native()); });
        auto* record = textures_.find(id.local);
        if (id.isNull())
        {
            if (expected != 0)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        else
        {
            if (!record || !record->current)
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
            const bool conflict = record->updating || record->current->revision_ != expected || expected == UINT64_MAX;
            if (conflict)
            {
                return cxx::unexpected(RenderError{kBusy});
            }
            const auto& previous = record->current->info_;
            const auto& candidate = info;
            const bool incompatible_consumers =
                record->bindings &&
                (previous.width != candidate.width || previous.height != candidate.height ||
                 previous.mip_count != candidate.mip_count || previous.pixel_format != candidate.pixel_format ||
                 previous.layers != candidate.layers);
            if (incompatible_consumers)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        auto descriptor = descriptors_->allocate();
        if (!descriptor)
        {
            return cxx::unexpected(descriptor.error());
        }
        auto content = std::make_unique<SampledImage>(std::move(image), std::move(*descriptor), info, expected + 1);
        cxx::scope_exit rollback_content(
            [&]() noexcept
            {
                if (content)
                {
                    content->release(session_.nativeDevice(), session_.allocator().native(), *descriptors_);
                }
            }
        );
        auto& version = *content;
        const auto device = session_.nativeDevice();
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = version.image_.handle();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = version.image_.format();
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, version.info_.mip_count, 0, 1};
        auto result = vkCreateImageView(device, &view, nullptr, &version.view_);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = float(version.info_.mip_count - 1);
        result = version.sampler_.tryCreate(device, sampler);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        // The descriptor becomes reachable only after the required upload submissions are accepted.
        descriptors_->write(version.descriptor_, version.view_, version.sampler_);
        if (id.isNull())
        {
            id = TextureId{id_, textures_.emplace()};
        }
        record = textures_.find(id.local);
        record->updating = true;
        return Candidate{*this, std::move(content), id};
    }

    void TextureStorage::rollback(Candidate& candidate) noexcept
    {
        auto& record = *textures_.find(candidate.texture.local);
        record.updating = false;
        if (!record.current)
        {
            textures_.erase(candidate.texture.local);
        }
        retire(candidate.version);
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureWrite input) noexcept
    {
        if (input.texture.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto source = input.source;
        auto prepared = prepareImage(input.info, source);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = reserve(std::move(prepared->first), input.info, input.texture, input.expected_revision);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        const TexturePublished result{candidate->texture, candidate->version->revision_};
        auto uploaded = co_await session_.uploads()
                            .writeImage(candidate->version->image_, input.source, std::move(prepared->second));
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kWorkStopped});
        }
        auto committed = co_await commit(*candidate);
        if (!committed)
        {
            co_return cxx::unexpected(committed.error());
        }
        co_return result;
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureRegionWrite input) noexcept
    {
        if (input.texture.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* record = textures_.find(input.texture.local);
        if (!record || !record->current)
        {
            co_return cxx::unexpected(RenderError{kWrongOwner});
        }
        if (record->updating || record->current->revision_ != input.expected_revision)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto& info = record->current->info_;
        auto prepared = prepareImage(info, input.source, input.region);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = reserve(std::move(prepared->first), info, input.texture, input.expected_revision);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        candidate->base = record->current.get();
        const TexturePublished result{input.texture, candidate->version->revision_};
        auto uploaded = co_await session_.uploads().writeImage(
            candidate->version->image_,
            input.source,
            std::move(prepared->second),
            &candidate->base->image_
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kWorkStopped});
        }
        auto committed = co_await commit(*candidate);
        if (!committed)
        {
            co_return cxx::unexpected(committed.error());
        }
        co_return result;
    }

    void TextureStorage::retire(std::unique_ptr<SampledImage>& value) noexcept
    {
        if (!value)
        {
            return;
        }
        session_.deferRelease(
            [value = std::move(value),
             device = session_.nativeDevice(),
             allocator = session_.allocator().native(),
             descriptors = descriptors_]() mutable noexcept
            {
                value->release(device, allocator, *descriptors);
                value.reset();
            }
        );
    }

    void TextureStorage::publish(Candidate& candidate) noexcept
    {
        auto& record = *textures_.find(candidate.texture.local);
        retire(record.current);
        record.current = std::move(candidate.version);
        record.updating = false;
        StorageSubscription::publish(record.bindings, resource(record.current.get()));
    }

    bool TextureStorage::hasDependents(TextureId id) const noexcept
    {
        return std::ranges::any_of(
            observers_,
            [id](const auto* observer) { return observer->affected(observer->receiver, id); }
        );
    }

    void TextureStorage::recordDependents(VkCommandBuffer command, TextureId id, std::uint32_t descriptor) noexcept
    {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        for (auto* observer : observers_)
        {
            observer->record(observer->receiver, command, id, descriptor);
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void TextureStorage::adoptDependents(TextureId id, std::uint32_t descriptor) noexcept
    {
        for (auto* observer : observers_)
        {
            observer->adopt(observer->receiver, id, descriptor);
        }
    }

    exec::task<RenderResult<void>> TextureStorage::commit(Candidate& candidate) noexcept
    {
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (hasDependents(candidate.texture))
        {
            std::optional<CommandBatch> batch;
            for (;;)
            {
                if (stop.stop_requested())
                {
                    co_return cxx::unexpected(RenderError{kWorkStopped});
                }
                auto ready = session_.queue().begin();
                if (ready)
                {
                    batch.emplace(std::move(*ready));
                    break;
                }
                if (ready.error().type != kCapacity)
                {
                    co_return cxx::unexpected(ready.error());
                }
                auto completed = co_await session_.queue().whenAvailable();
                if (!completed)
                {
                    co_return cxx::unexpected(completed.error());
                }
            }
            // Each professional consumer records its own fields. No Sender suspends between begin,
            // record, Submit and adoption; failure preserves every old CPU index and Texture version.
            recordDependents(batch->native(), candidate.texture, candidate.version->descriptorIndex());
            auto accepted = std::move(*batch).submit();
            if (!accepted)
            {
                co_return cxx::unexpected(accepted.error());
            }
            adoptDependents(candidate.texture, candidate.version->descriptorIndex());
        }
        publish(candidate);
        co_return RenderResult<void>{};
    }

    exec::task<RenderResult<Ack>> TextureStorage::release(TextureId id, std::uint64_t expected) noexcept
    {
        auto* record = id.storage == id_ ? textures_.find(id.local) : nullptr;
        if (!record || !record->current)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (record->updating || record->current->revision_ != expected)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        record->updating = true;
        cxx::scope_exit reset([&]() noexcept { record->updating = false; });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (hasDependents(id))
        {
            std::optional<CommandBatch> batch;
            for (;;)
            {
                if (stop.stop_requested())
                {
                    co_return cxx::unexpected(RenderError{kWorkStopped});
                }
                auto ready = session_.queue().begin();
                if (ready)
                {
                    batch.emplace(std::move(*ready));
                    break;
                }
                if (ready.error().type != kCapacity)
                {
                    co_return cxx::unexpected(ready.error());
                }
                auto completed = co_await session_.queue().whenAvailable();
                if (!completed)
                {
                    co_return cxx::unexpected(completed.error());
                }
            }
            recordDependents(batch->native(), id, 0);
            auto accepted = std::move(*batch).submit();
            if (!accepted)
            {
                co_return cxx::unexpected(accepted.error());
            }
            adoptDependents(id, 0);
        }
        disconnect(*record);
        retire(record->current);
        reset.release();
        textures_.erase(id.local);
        co_return Ack{};
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
