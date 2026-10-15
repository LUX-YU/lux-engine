#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "Native.hpp"
#include <algorithm>

namespace lux::render::vulkan
{
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
        static const StorageFactory factory{
            &lux::render::TextureStorage::description, EGraphicsApi::VULKAN, &ops,
            [](void* device, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto value = vulkan::TextureStorage::create(*static_cast<vulkan::DeviceSession*>(device),
                    *static_cast<const TextureStorageConfig*>(config));
                if (!value) return cxx::unexpected(value.error());
                return NativeStorage{value->release(), [](void* object) noexcept {
                    delete static_cast<vulkan::TextureStorage*>(object);
                }};
            },
            [](const void* value, const void* config) noexcept {
                return static_cast<const vulkan::TextureStorage*>(value)->compatibleConfig(
                    *static_cast<const TextureStorageConfig*>(config));
            }, {}, nullptr, [](void* value) noexcept { static_cast<TextureStorage*>(value)->collect(); }
        };
        return factory;
    }

    TextureStorage* DeviceSession::textures() const noexcept
    {
        auto id = resources_->find(resources_->root(), "default", lux::render::TextureStorage::description);
        return id ? static_cast<TextureStorage*>(resources_->native(*id)) : nullptr;
    }

    RenderResult<void> DeviceSession::configureTextures(std::size_t capacity, std::uint64_t bytes) noexcept
    {
        if (capacity > UINT32_MAX) return cxx::unexpected(RenderError{kInvalidArgument});
        const TextureStorageConfig config{static_cast<std::uint32_t>(capacity), bytes};
        auto value = resources_->getOrCreate(resources_->root(), "default", lux::render::TextureStorage::description, &config);
        if (!value) return cxx::unexpected(value.error());
        return {};
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
        const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr, 1, &flags};
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            std::uint32_t(capacity_), VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.pNext = &binding_flags;
        layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        auto result = LUX_NATIVE("texture_layout", vkCreateDescriptorSetLayout(device, &layout, nullptr, &layout_));
        if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::uint32_t(capacity_)};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        result = LUX_NATIVE("texture_pool", vkCreateDescriptorPool(device, &pool, nullptr, &pool_));
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

    const TextureVersion* TextureStorage::find(TextureId id) const noexcept
    {
        const auto* record = textures_.find(id);
        return record ? record->current : nullptr;
    }

    RenderResult<TextureVersion*> TextureStorage::acquire(TextureId id) noexcept
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

    RenderResult<TexturePublished> TextureStorage::reserve(
        TextureUpload& upload, TextureId id, std::uint64_t expected) noexcept
    {
        if (!upload.unstartedIn(session_) || upload.storage_) return cxx::unexpected(RenderError{kWrongOwner});
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
            const auto& candidate = upload.info_;
            const bool incompatible_consumers = record->current->leases_ &&
                (previous.width != candidate.width || previous.height != candidate.height ||
                 previous.mip_count != candidate.mip_count || previous.pixel_format != candidate.pixel_format ||
                 previous.layers != candidate.layers);
            if (incompatible_consumers) return cxx::unexpected(RenderError{kUnsupported});
        }
        collect();
        const auto bytes = session_.memoryBinding(upload.image()).size;
        const bool full = versions_.size() == capacity_ || (id.isNull() && textures_.size() == capacity_) ||
            bytes > byte_limit_ - resident_bytes_;
        if (full) return cxx::unexpected(RenderError{kCapacity});
        auto slot = versions_.tryEmplacePrepared();
        if (!slot) return cxx::unexpected(RenderError{kCapacity});
        auto& version = *versions_.find(*slot);
        version.key_ = *slot;
        version.info_ = upload.info_;
        version.image_ = session_.borrow(upload.transfer_->image_);
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
        upload.storage_ = this;
        upload.storage_id_ = session_.resources().identityOf(this);
        session_.resources().retain(upload.storage_id_);
        upload.version_ = &version;
        upload.texture_ = id;
        return TexturePublished{id, version.revision_};
    }

    void TextureStorage::rollback(TextureUpload& upload) noexcept
    {
        auto& record = *textures_.find(upload.texture_);
        record.updating = false;
        if (!record.current) textures_.erase(upload.texture_);
        destroy(*upload.version_); // descriptors/views before the Upload-owned image
        upload.storage_ = nullptr;
        upload.version_ = nullptr;
    }

    RenderResult<std::unique_ptr<TextureUpload>> TextureStorage::uploadRegion(TextureId id, std::uint64_t expected,
        TextureRegion region, std::span<const std::byte> source) noexcept
    {
        auto* record = textures_.find(id);
        if (!record || !record->current) return cxx::unexpected(RenderError{kWrongOwner});
        if (record->updating || record->current->revision_ != expected) return cxx::unexpected(RenderError{kBusy});
        const auto last = record->current->info_.mip_ranges[record->current->info_.mip_count - 1];
        if (auto result = admit(last.offset + last.size, false); !result) return cxx::unexpected(result.error());
        auto upload = session_.prepareTextureUpload(record->current->info_, source, region);
        if (!upload) return cxx::unexpected(upload.error());
        auto publication = reserve(**upload, id, expected);
        if (!publication) return cxx::unexpected(publication.error());
        (*upload)->base_ = record->current;
        (*upload)->transfer_->clone_ = record->current->image_;
        ++record->current->leases_;
        return upload;
    }

    void TextureStorage::publish(TextureUpload& upload) noexcept
    {
        auto& record = *textures_.find(upload.texture_);
        auto& version = *upload.version_;
        version.permit_ = std::move(*upload.transfer_->takeImage());
        version.last_use_ = upload.transfer_->lastUse();
        version.reserved_ = false;
        version.live_ = true;
        if (record.current) record.current->live_ = false;
        record.current = &version;
        record.updating = false;
        session_.refreshTexture(upload.texture_, version);
        upload.storage_ = nullptr;
        upload.version_ = nullptr;
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
            const bool completed = !version.last_use_ || RetirementQueue::completed(*version.last_use_);
            if (!version.live_ && !version.reserved_ && !version.leases_ && completed)
            {
                destroy(version);
                // StableSlotMap's dense-index iterator now denotes the swapped-in element (or end).
            }
            else ++it;
        }
    }

    void TextureStorage::destroy(TextureVersion& version) noexcept
    {
        const auto device = session_.device().native();
        if (version.view_) LUX_DESTROY("image_view", vkDestroyImageView(device, version.view_, nullptr));
        if (version.sampler_) LUX_DESTROY("texture_sampler", vkDestroySampler(device, version.sampler_, nullptr));
        if (version.permit_)
        {
            auto result = session_.releaseCompleted(version.permit_, *version.last_use_);
            if (!result) std::terminate();
        }
        resident_bytes_ -= version.bytes_;
        versions_.erase(version.key_);
    }
}
