#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <bit>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "Native.hpp"
#include "Shader.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <algorithm>

namespace lux::render::vulkan
{
    namespace
    {
        struct Format final { VkFormat native; std::uint32_t extent, bytes; bool srgb; };
        Format format(rdesc::ETexturePixelFormat value) noexcept
        {
            switch (value)
            {
            case rdesc::ETexturePixelFormat::RGBA8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false};
            case rdesc::ETexturePixelFormat::RGBA8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 1, 4, true};
            case rdesc::ETexturePixelFormat::BC1_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8, true};
            default: return {};
            }
        }
    }

    RenderResult<std::unique_ptr<Transfer>> TextureStorage::prepareImage(
        const rdesc::TextureInfo& info, std::span<const std::byte> source, std::optional<TextureRegion> region, Backing* base
    ) noexcept
    {
        auto& session = session_;
        const auto layout = format(info.pixel_format);
        const auto max_dimension = session.device().properties().limits.maxImageDimension2D;
        const bool bad_extent = info.width <= 0 || info.height <= 0 ||
            std::uint32_t(info.width) > max_dimension || std::uint32_t(info.height) > max_dimension;
        const bool bad_count = info.layers != 1 || info.mip_count == 0 || info.mip_count > rdesc::kTextureMaxMipCount;
        const bool bad_color = info.color_space == rdesc::ETextureColorSpace::UNKNOWN ||
            info.color_space > rdesc::ETextureColorSpace::DATA ||
            (layout.srgb != (info.color_space == rdesc::ETextureColorSpace::SRGB));
        const bool premultiplied_srgb = layout.srgb &&
            rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
        const bool bad_flags = (info.flags & ~7u) != 0 ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::COMPRESSED) != (layout.extent == 4)) ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::NO_MIPS) && info.mip_count != 1);
        if (layout.native == VK_FORMAT_UNDEFINED) return cxx::unexpected(RenderError{kUnsupported});
        if (bad_extent || bad_count || bad_color || bad_flags || info.channel != 4)
            return cxx::unexpected(RenderError{kInvalidArgument});
        if (layout.extent == 4 && !session.device().textureCompressionBc()) return cxx::unexpected(RenderError{kUnsupported});
        if (premultiplied_srgb) return cxx::unexpected(RenderError{kUnsupported});
        const auto max_mips = std::bit_width(std::max(std::uint32_t(info.width), std::uint32_t(info.height)));
        if (info.mip_count > max_mips) return cxx::unexpected(RenderError{kInvalidArgument});
        std::uint64_t expected_offset = 0;
        for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
        {
            const auto width = std::max(1u, std::uint32_t(info.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(info.height) >> mip);
            const auto rows = (height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = std::uint64_t((width + layout.extent - 1) / layout.extent) * layout.bytes;
            const auto size = cxx::checkedMul<std::uint64_t>(row, std::uint64_t(rows));
            if (!size) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto& input = info.mip_ranges[mip];
            const bool mismatch = input.width != width || input.height != height || input.size != *size ||
                input.offset != expected_offset;
            if (mismatch) return cxx::unexpected(RenderError{kInvalidArgument});
            auto total = cxx::checkedAdd(expected_offset, *size);
            if (!total) return cxx::unexpected(RenderError{kInvalidArgument});
            expected_offset = *total;
        }
        if (region)
        {
            if (region->mip >= info.mip_count) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto& mip = info.mip_ranges[region->mip];
            const bool invalid_offset = region->x >= mip.width || region->y >= mip.height;
            if (invalid_offset) return cxx::unexpected(RenderError{kInvalidArgument});
            const bool invalid_range = !region->width || !region->height || region->width > mip.width - region->x ||
                region->height > mip.height - region->y;
            const bool invalid_blocks = region->x % layout.extent || region->y % layout.extent ||
                (region->width % layout.extent && region->width != mip.width - region->x) ||
                (region->height % layout.extent && region->height != mip.height - region->y);
            if (invalid_range || invalid_blocks) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto rows = (region->height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = ((region->width + layout.extent - 1) / layout.extent) * layout.bytes;
            const bool invalid_pitch = region->row_pitch < row || region->row_pitch % layout.bytes;
            if (invalid_pitch) return cxx::unexpected(RenderError{kInvalidArgument});
            auto stride = cxx::checkedMul<std::uint64_t>(rows - 1, region->row_pitch);
            if (!stride) return cxx::unexpected(RenderError{kInvalidArgument});
            auto total = cxx::checkedAdd(*stride, row);
            if (!total) return cxx::unexpected(RenderError{kInvalidArgument});
            expected_offset = *total;
        }
        if (expected_offset != source.size() || source.empty()) return cxx::unexpected(RenderError{kInvalidArgument});
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(session.device().physical(), layout.native, &properties);
        constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
            return cxx::unexpected(RenderError{kUnsupported});
        const ImageDescription description{{std::uint32_t(info.width), std::uint32_t(info.height)}, layout.native,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT, info.mip_count, 1};
        std::vector<ImageTransferRegion> regions;
        if (region)
            regions.push_back({0, region->row_pitch, layout.extent, layout.bytes,
                {VK_IMAGE_ASPECT_COLOR_BIT, region->mip, 0, 1},
                {std::int32_t(region->x), std::int32_t(region->y), 0}, {region->width, region->height, 1}});
        else
            for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
            {
                const auto& input = info.mip_ranges[mip];
                const auto pitch = std::uint64_t((input.width + layout.extent - 1) / layout.extent) * layout.bytes;
                regions.push_back({input.offset, pitch, layout.extent, layout.bytes,
                    {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1}, {}, {input.width, input.height, 1}});
            }
        return session.uploadImage(description, source, regions, base ? base->image_ : nullptr,
            region.has_value(), base ? &base->last_use_ : nullptr);
    }

    // Exclusive reservation of an existing Storage candidate. No transfer, queue or async phase is owned here.
    struct TextureStorage::Candidate final
    {
        TextureStorage& storage;
        Backing* version;
        TextureId texture;
        Backing* base = nullptr;
        StorageInstanceId storage_id;

        ~Candidate() noexcept
        {
            if (version) storage.rollback(*this);
            if (base) --base->leases_;
            storage.session_.resources().release(storage_id);
        }
    };

    struct TextureStorage::Use final
    {
        TextureStorage* storage;
        TextureId id;
        TextureStorage::Backing* backing;
        PreparedStorageBinding* binding;
        std::uint32_t lod;

        void update(TextureStorage::Backing& value) noexcept
        {
            if (backing) --backing->leases_;
            backing = &value;
            ++backing->leases_;
            binding->parameters = {float(value.descriptorIndex()), float(value.info().color_space), float(lod), 0};
            binding->last_use = &value.last_use_;
        }

        ~Use() noexcept
        {
            --backing->leases_;
            std::erase(storage->uses_, this);
        }
    };

    RenderResult<void> TextureStorage::prepare(const NativeResourcePlan& plan, std::span<const std::byte> bytes,
        PreparedStorageBinding& binding) noexcept
    {
        cxx::BinaryReader reader(bytes, bytes.size()); // schema/length already checked at Attach boundary
        auto argument = DataSchema<TextureBinding>::decode(reader,
            EncodedDataView{DataSchema<TextureBinding>::schema, bytes, {}});
        auto* record = textures_.find(argument->texture);
        if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
        auto& backing = *record->current;
        const auto* expected = std::get_if<GraphImageDesc>(&plan.description);
        const auto& actual = backing.info();
        if (!expected) return cxx::unexpected(RenderError{kStorageMismatch});
        const auto format = expected->format == rdesc::ETextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM :
            expected->format == rdesc::ETextureFormat::RGBA8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const bool mismatch = expected->width != actual.width || expected->height != actual.height ||
            expected->mips != actual.mip_count || expected->layers != actual.layers || format != backing.image().format() ||
            argument->lod >= actual.mip_count;
        if (mismatch) return cxx::unexpected(RenderError{kStorageMismatch});
        auto use = std::make_unique<Use>(this, argument->texture, nullptr, &binding, argument->lod);
        use->update(backing);
        binding.descriptors = descriptors_;
        binding.provides_parameters = true;
        uses_.push_back(use.get());
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    TextureStorage::TextureStorage(DeviceSession& session, std::size_t capacity, std::uint64_t bytes) noexcept
        : session_(session), capacity_(capacity), byte_limit_(bytes)
    {
        textures_.reserve(capacity);
        versions_.reserve(capacity);
    }

    StorageFactory textureStorageFactory() noexcept
    {
        static const lux::render::TextureStorage::Ops ops{
            [](const void* value, TextureId id) noexcept -> RenderResult<TexturePublished> {
                auto* record = static_cast<const vulkan::TextureStorage*>(value)->find(id);
                if (!record) return cxx::unexpected(RenderError{kStorageDomain});
                return TexturePublished{id, record->revision()};
            },
            [](void* value, TextureId id, std::uint64_t revision) noexcept {
                return static_cast<vulkan::TextureStorage*>(value)->release(id, revision);
            }
        };
        static const StorageBindingOps binding_ops{
            [](void* storage, const NativeResourcePlan& plan, std::span<const std::byte> argument,
                PreparedStorageBinding& binding) noexcept {
                return static_cast<TextureStorage*>(storage)->prepare(plan, argument, binding);
            }, [](const void* configuration) noexcept {
            return configuration ? static_cast<const TextureStorageConfig*>(configuration)->capacity : 0;
        }};
        static const auto compatible = +[](const vulkan::TextureStorage& value, const TextureStorageConfig& config) noexcept {
            return value.compatibleConfig(config);
        };
        static const auto factory = makeStorageFactory<lux::render::TextureStorage, vulkan::TextureStorage,
            DeviceSession, &vulkan::TextureStorage::create, compatible>(ops, &binding_ops,
                [](void* value) noexcept { static_cast<TextureStorage*>(value)->collect(); });
        return factory;
    }

    RenderResult<std::unique_ptr<TextureStorage>> TextureStorage::create(
        DeviceSession& session, const TextureStorageConfig& config
    ) noexcept
    {
        const bool invalid = config.capacity < 16 || config.capacity > 4096 || config.capacity % 16 != 0 ||
            config.byte_limit == 0;
        if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
        if (!session.device().textureDescriptors()) return cxx::unexpected(RenderError{kUnsupported});
        auto storage = std::unique_ptr<TextureStorage>(new TextureStorage(session, config.capacity, config.byte_limit));
        if (auto result = storage->initialize(); !result) return cxx::unexpected(result.error());
        return storage;
    }

    RenderResult<void> TextureStorage::initialize() noexcept
    {
        VkPhysicalDeviceDescriptorIndexingProperties limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits};
        vkGetPhysicalDeviceProperties2(session_.device().physical(), &properties);
        const auto maximum = std::min({limits.maxDescriptorSetUpdateAfterBindSamplers,
            limits.maxDescriptorSetUpdateAfterBindSampledImages, limits.maxPerStageDescriptorUpdateAfterBindSamplers,
            limits.maxPerStageDescriptorUpdateAfterBindSampledImages, limits.maxUpdateAfterBindDescriptorsInAllPools});
        if (capacity_ > maximum) return cxx::unexpected(RenderError{kUnsupported});
        const auto device = session_.device().native();
        auto layout = createDescriptorLayout(session_.device(), lux::render::TextureStorage::description.bindings,
            std::uint32_t(capacity_), "texture_layout");
        if (!layout) return cxx::unexpected(layout.error());
        layout_ = *layout;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::uint32_t(capacity_)};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        auto result = LUX_NATIVE("texture_pool", vkCreateDescriptorPool(device, &pool, nullptr, &pool_));
        if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout_;
        result = LUX_NATIVE("texture_set", vkAllocateDescriptorSets(device, &allocate, &descriptors_));
        return result == VK_SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(result));
    }

    TextureStorage::~TextureStorage() noexcept
    {
        // DeviceSession has drained and destroyed Context leases before reaching this owner.
        for (auto& version : versions_)
            if (version.leases_ || version.reserved_) std::terminate();
        while (!versions_.empty()) destroy(*versions_.begin());
        const auto device = session_.device().native();
        if (pool_) LUX_DESTROY("texture_pool", vkDestroyDescriptorPool(device, pool_, nullptr));
        if (layout_) LUX_DESTROY("texture_layout", vkDestroyDescriptorSetLayout(device, layout_, nullptr));
    }

    const TextureStorage::Backing* TextureStorage::find(TextureId id) const noexcept
    {
        const auto* record = textures_.find(id);
        return record ? record->current : nullptr;
    }

    RenderResult<TextureStorage::Backing*> TextureStorage::acquire(TextureId id) noexcept
    {
        auto* record = textures_.find(id);
        if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
        ++record->current->leases_;
        return record->current;
    }

    RenderResult<void> TextureStorage::admit(std::uint64_t minimum_bytes, bool creates_identity) noexcept
    {
        collect();
        const bool full = versions_.size() == capacity_ || (creates_identity && textures_.size() == capacity_) ||
            minimum_bytes > byte_limit_ - resident_bytes_;
        if (full) return cxx::unexpected(RenderError{kCapacity});
        return {};
    }

    RenderResult<std::unique_ptr<TextureStorage::Candidate>> TextureStorage::reserve(
        Transfer& transfer, const rdesc::TextureInfo& info, TextureId id, std::uint64_t expected) noexcept
    {
        auto* record = textures_.find(id);
        if (id.isNull())
        {
            if (expected != 0) return cxx::unexpected(RenderError{kInvalidArgument});
        }
        else
        {
            if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
            const bool conflict = record->updating || record->current->revision_ != expected || expected == UINT64_MAX;
            if (conflict) return cxx::unexpected(RenderError{kBusy});
            const auto& previous = record->current->info_;
            const auto& candidate = info;
            const bool incompatible_consumers = record->current->leases_ &&
                (previous.width != candidate.width || previous.height != candidate.height ||
                 previous.mip_count != candidate.mip_count || previous.pixel_format != candidate.pixel_format ||
                 previous.layers != candidate.layers);
            if (incompatible_consumers) return cxx::unexpected(RenderError{kUnsupported});
        }
        collect();
        const auto bytes = session_.memoryBinding(transfer.image()).size;
        const bool full = versions_.size() == capacity_ || (id.isNull() && textures_.size() == capacity_) ||
            bytes > byte_limit_ - resident_bytes_;
        if (full) return cxx::unexpected(RenderError{kCapacity});
        auto slot = versions_.tryEmplacePrepared();
        if (!slot) return cxx::unexpected(RenderError{kCapacity});
        auto& version = *versions_.find(*slot);
        version.key_ = *slot;
        version.info_ = info;
        version.image_ = &transfer.image();
        version.bytes_ = bytes;
        version.revision_ = expected + 1;
        resident_bytes_ += bytes;
        resident_peak_ = std::max(resident_peak_, resident_bytes_);
        const auto device = session_.device().native();
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = version.image_->native();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = version.image_->format();
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, version.info_.mip_count, 0, 1};
        auto result = LUX_NATIVE("image_view", vkCreateImageView(device, &view, nullptr, &version.view_));
        if (result != VK_SUCCESS) { destroy(version); return cxx::unexpected(nativeError(result)); }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = float(version.info_.mip_count - 1);
        result = LUX_NATIVE("texture_sampler", vkCreateSampler(device, &sampler, nullptr, &version.sampler_));
        if (result != VK_SUCCESS) { destroy(version); return cxx::unexpected(nativeError(result)); }
        // This unused descriptor is not reachable by any Context until publish after final Graphics completion.
        VkDescriptorImageInfo image{version.sampler_, version.view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptors_;
        write.dstBinding = 0;
        write.dstArrayElement = slot->index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        if (id.isNull()) id = textures_.emplace();
        record = textures_.find(id);
        record->updating = true;
        const auto storage_id = session_.resources().identityOf(this);
        session_.resources().retain(storage_id);
        return std::make_unique<Candidate>(*this, &version, id, nullptr, storage_id);
    }

    void TextureStorage::rollback(Candidate& candidate) noexcept
    {
        auto& record = *textures_.find(candidate.texture);
        record.updating = false;
        if (!record.current) textures_.erase(candidate.texture);
        destroy(*candidate.version); // descriptors/views before the Transfer-owned image
        candidate.version = nullptr;
    }

    TextureStorage::UploadResult TextureStorage::upload(const TextureWrite& input) noexcept
    {
        const auto source = input.source.bytes().view();
        if (auto admitted = admit(source.size(), input.texture.isNull()); !admitted)
            return cxx::unexpected(admitted.error());
        auto transfer = prepareImage(input.info, source);
        if (!transfer) return cxx::unexpected(transfer.error());
        auto candidate = reserve(**transfer, input.info, input.texture, input.expected_revision);
        if (!candidate) return cxx::unexpected(candidate.error());
        const TexturePublished result{(*candidate)->texture, (*candidate)->version->revision_};
        auto work = GpuWork::pending<[](Candidate& value, Transfer& transfer, DeviceSession&) noexcept -> RenderResult<void> {
            value.storage.publish(value, transfer);
            return {};
        }>(session_, *transfer, *candidate);
        if (!work) return cxx::unexpected(work.error());
        return std::pair{result, std::move(*work)};
    }

    TextureStorage::UploadResult TextureStorage::upload(const TextureRegionWrite& input) noexcept
    {
        auto* record = textures_.find(input.texture);
        if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
        if (record->updating || record->current->revision_ != input.expected_revision)
            return cxx::unexpected(RenderError{kBusy});
        const auto& info = record->current->info_;
        const auto last = info.mip_ranges[info.mip_count - 1];
        if (auto admitted = admit(last.offset + last.size, false); !admitted) return cxx::unexpected(admitted.error());
        auto transfer = prepareImage(info, input.source.bytes().view(), input.region, record->current);
        if (!transfer) return cxx::unexpected(transfer.error());
        auto candidate = reserve(**transfer, info, input.texture, input.expected_revision);
        if (!candidate) return cxx::unexpected(candidate.error());
        (*candidate)->base = record->current;
        ++record->current->leases_;
        const TexturePublished result{input.texture, (*candidate)->version->revision_};
        auto work = GpuWork::pending<[](Candidate& value, Transfer& transfer, DeviceSession&) noexcept -> RenderResult<void> {
            value.storage.publish(value, transfer);
            return {};
        }>(session_, *transfer, *candidate);
        if (!work) return cxx::unexpected(work.error());
        return std::pair{result, std::move(*work)};
    }

    void TextureStorage::publish(Candidate& candidate, Transfer& transfer) noexcept
    {
        auto& record = *textures_.find(candidate.texture);
        auto& version = *candidate.version;
        version.permit_ = std::move(*transfer.takeImage());
        version.last_use_ = transfer.requirements();
        version.reserved_ = false;
        version.live_ = true;
        if (record.current) record.current->live_ = false;
        record.current = &version;
        record.updating = false;
        for (auto* use : uses_) if (use->id == candidate.texture) use->update(version);
        candidate.version = nullptr;
    }

    RenderResult<void> TextureStorage::release(TextureId id, std::uint64_t expected) noexcept
    {
        auto* record = textures_.find(id);
        if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
        if (record->updating || record->current->revision_ != expected) return cxx::unexpected(RenderError{kBusy});
        record->current->live_ = false;
        textures_.erase(id);
        collect();
        return {};
    }

    void TextureStorage::collect() noexcept
    {
        for (auto it = versions_.begin(); it != versions_.end();)
        {
            auto& version = *it;
            const bool completed = version.last_use_.completed();
            if (!version.live_ && !version.reserved_ && !version.leases_ && completed)
            {
                destroy(version);
                // StableSlotMap's dense-index iterator now denotes the swapped-in element (or end).
            }
            else ++it;
        }
    }

    void TextureStorage::destroy(TextureStorage::Backing& version) noexcept
    {
        const auto device = session_.device().native();
        if (version.view_) LUX_DESTROY("image_view", vkDestroyImageView(device, version.view_, nullptr));
        if (version.sampler_) LUX_DESTROY("texture_sampler", vkDestroySampler(device, version.sampler_, nullptr));
        if (version.permit_)
        {
            auto result = session_.releaseCompleted(version.permit_, version.last_use_);
            if (!result) std::terminate();
        }
        resident_bytes_ -= version.bytes_;
        versions_.erase(version.key_);
    }
}

namespace lux::render
{
    StorageTraits<vulkan::TextureStorage>::Result
    StorageTraits<vulkan::TextureStorage>::upload(vulkan::TextureStorage& storage, const TextureWrite& input) noexcept
    {
        return storage.upload(input);
    }

    StorageTraits<vulkan::TextureStorage>::Result
    StorageTraits<vulkan::TextureStorage>::upload(vulkan::TextureStorage& storage,
        const TextureRegionWrite& input) noexcept
    {
        return storage.upload(input);
    }
}
