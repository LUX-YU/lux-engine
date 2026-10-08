#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    namespace
    {
        struct ShadowMappedBuffer
        {
            VmaBuffer owner;
            void* mapped{};
        };

        Expected<ShadowMappedBuffer> createShadowBuffer(
            VmaAllocator allocator,
            VkDeviceSize size,
            VkBufferUsageFlags usage
        ) noexcept
        {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaBuffer::Allocation allocation{allocator};
            VmaAllocationInfo mapping{};
            const auto status = vmaCreateBuffer(
                allocator,
                &info,
                &allocation_info,
                &allocation.buffer,
                &allocation.allocation,
                &mapping
            );
            if (status != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
            }
            auto owner = VmaBuffer::adopt(allocation);
            if (!mapping.pMappedData)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            return ShadowMappedBuffer{std::move(owner), mapping.pMappedData};
        }
    } // namespace

    struct ShadowResources::Backing
    {
        struct Frame
        {
            ShadowMappedBuffer slices;
            ShadowMappedBuffer config;
            ShadowMappedBuffer spot_map;
            ShadowMappedBuffer point_map;
        };

        VmaImage atlas;
        ImageViewOwner view; // Released before its image.
        std::vector<Frame> frames;
        uint32_t resolution{};
        uint32_t pages{};
        uint32_t slices{};
    };

    Expected<std::unique_ptr<ShadowResources::Backing>> ShadowResources::createBacking(
        DeviceContext& device,
        uint32_t frames,
        uint32_t resolution,
        uint32_t pages,
        uint32_t slices
    ) noexcept
    {
        const bool is_invalid_frames = frames == 0 || frames > kMaxFramesInFlight;
        const bool is_invalid_extent = resolution == 0 || pages == 0 || slices == 0;
        if (is_invalid_frames || is_invalid_extent)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }

        auto candidate = std::make_unique<Backing>();
        candidate->resolution = resolution;
        candidate->pages = pages;
        candidate->slices = slices;
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_D32_SFLOAT;
        image_info.extent = {resolution, resolution, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = pages;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        auto image = VmaImage::create(device.vmaAllocator(), image_info, allocation_info);
        if (!image)
        {
            return lux::cxx::unexpected(image.error());
        }
        candidate->atlas = std::move(*image);
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = candidate->atlas.image();
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_info.format = image_info.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, pages};
        auto view = ImageViewOwner::create(device.logicalDevice(), view_info);
        if (!view)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(view.error()));
        }
        candidate->view = std::move(*view);
        candidate->frames.reserve(frames);
        const VkDeviceSize slice_bytes = static_cast<VkDeviceSize>(sizeof(ShadowSliceGPU)) * slices;
        const VkDeviceSize map_bytes = sizeof(int32_t) * kShadowLightMapCapacity;
        for (uint32_t fi = 0; fi < frames; ++fi)
        {
            auto slice = createShadowBuffer(device.vmaAllocator(), slice_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (!slice)
            {
                return lux::cxx::unexpected(slice.error());
            }
            auto config =
                createShadowBuffer(device.vmaAllocator(), sizeof(ShadowConfigGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            if (!config)
            {
                return lux::cxx::unexpected(config.error());
            }
            auto spot = createShadowBuffer(device.vmaAllocator(), map_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (!spot)
            {
                return lux::cxx::unexpected(spot.error());
            }
            auto point = createShadowBuffer(device.vmaAllocator(), map_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (!point)
            {
                return lux::cxx::unexpected(point.error());
            }
            std::fill_n(static_cast<int32_t*>(spot->mapped), kShadowLightMapCapacity, -1);
            std::fill_n(static_cast<int32_t*>(point->mapped), kShadowLightMapCapacity, -1);
            const ShadowConfigGPU empty_config{};
            std::memcpy(config->mapped, &empty_config, sizeof(empty_config));
            for (const auto* buffer : {&*spot, &*point, &*config})
            {
                const auto status =
                    vmaFlushAllocation(device.vmaAllocator(), buffer->owner.allocation(), 0, VK_WHOLE_SIZE);
                if (status != VK_SUCCESS)
                {
                    return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
                }
            }
            candidate->frames.push_back({std::move(*slice), std::move(*config), std::move(*spot), std::move(*point)});
        }
        return candidate;
    }

    ShadowResources::CreateResult ShadowResources::create(const CreateInfo& info) noexcept
    {
        const bool is_invalid_frames = info.frames_in_flight == 0 || info.frames_in_flight > kMaxFramesInFlight;
        const bool is_invalid_sets =
            info.domain_sets.size() != info.frames_in_flight ||
            std::ranges::any_of(info.domain_sets, [](auto set) { return set == VK_NULL_HANDLE; });
        const bool is_invalid_offset =
            info.domain_binding_offset > UINT32_MAX - static_cast<uint32_t>(ELightSetBindings::SHADOW_POINT_MAP);
        const auto layout = info.descriptors.layout(info.layout_id);
        const bool is_invalid_layout = layout == VK_NULL_HANDLE;
        if (is_invalid_frames || is_invalid_sets || is_invalid_offset || is_invalid_layout)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        DomainWriteTarget domain;
        if (auto bound = domain.set(info.domain_sets, info.domain_binding_offset); !bound)
        {
            return lux::cxx::unexpected(bound.error());
        }
        auto backing = createBacking(
            info.device,
            info.frames_in_flight,
            info.atlas_page_resolution,
            info.atlas_page_count,
            info.max_shadow_slices
        );
        if (!backing)
        {
            return lux::cxx::unexpected(backing.error());
        }
        auto sampler = info.descriptors.sampler(SamplerDesc::shadowCompare());
        if (!sampler)
        {
            return lux::cxx::unexpected(sampler.error());
        }
        const std::vector layouts(info.frames_in_flight, layout);
        auto sets = info.arena.allocateBatch(layouts);
        if (!sets)
        {
            return lux::cxx::unexpected(sets.error());
        }
        auto result = std::unique_ptr<ShadowResources>(
            new ShadowResources(info, std::move(domain), *sampler, std::move(*backing), std::move(*sets))
        );
        result->writeDescriptors();
        return result;
    }

    ShadowResources::ShadowResources(
        const CreateInfo& info,
        DomainWriteTarget domain,
        VkSampler sampler,
        std::unique_ptr<Backing> backing,
        std::vector<VkDescriptorSet> sets
    ) noexcept
        : device_(info.device), descriptor_svc_(info.descriptors), domain_(std::move(domain)), shadow_sampler_(sampler),
          shadow_ds_layout_id_(info.layout_id), shadow_ds_per_fif_(std::move(sets)), backing_(std::move(backing))
    {
    }

    ShadowResources::~ShadowResources() noexcept = default;

    VkImage ShadowResources::atlasImage() const noexcept
    {
        return backing_->atlas.image();
    }

    VkImageView ShadowResources::atlasView() const noexcept
    {
        return backing_->view.get();
    }

    VmaAllocation ShadowResources::atlasAllocation() const noexcept
    {
        return backing_->atlas.allocation();
    }

    uint32_t ShadowResources::framesInFlight() const noexcept
    {
        return static_cast<uint32_t>(backing_->frames.size());
    }

    uint32_t ShadowResources::atlasPageResolution() const noexcept
    {
        return backing_->resolution;
    }

    uint32_t ShadowResources::atlasPageCount() const noexcept
    {
        return backing_->pages;
    }

    uint32_t ShadowResources::maxSlices() const noexcept
    {
        return backing_->slices;
    }

    VkBuffer ShadowResources::sliceBuffer(uint32_t fi) const noexcept
    {
        return backing_->frames[fi % framesInFlight()].slices.owner.buffer();
    }

    VkBuffer ShadowResources::configBuffer(uint32_t fi) const noexcept
    {
        return backing_->frames[fi % framesInFlight()].config.owner.buffer();
    }

    VkBuffer ShadowResources::spotMapBuffer(uint32_t fi) const noexcept
    {
        return backing_->frames[fi % framesInFlight()].spot_map.owner.buffer();
    }

    VkBuffer ShadowResources::pointMapBuffer(uint32_t fi) const noexcept
    {
        return backing_->frames[fi % framesInFlight()].point_map.owner.buffer();
    }

    void ShadowResources::writeDescriptors() noexcept
    {
        const VkDeviceSize slice_bytes = static_cast<VkDeviceSize>(sizeof(ShadowSliceGPU)) * maxSlices();
        const VkDeviceSize map_bytes = sizeof(int32_t) * kShadowLightMapCapacity;
        const VkDescriptorImageInfo image{
            shadow_sampler_,
            atlasView(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        };
        for (uint32_t fi = 0; fi < framesInFlight(); ++fi)
        {
            const VkDescriptorBufferInfo slices{sliceBuffer(fi), 0, slice_bytes};
            const VkDescriptorBufferInfo config{configBuffer(fi), 0, sizeof(ShadowConfigGPU)};
            const VkDescriptorBufferInfo spot{spotMapBuffer(fi), 0, map_bytes};
            const VkDescriptorBufferInfo point{pointMapBuffer(fi), 0, map_bytes};
            const auto buffer_write =
                [](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo& buffer)
            {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = type;
                write.pBufferInfo = &buffer;
                return write;
            };
            const auto image_write = [&image](VkDescriptorSet set, uint32_t binding)
            {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image;
                return write;
            };
            const auto domain = domain_.setFor(fi);
            const auto binding = [this](ELightSetBindings value)
            { return domain_.binding(static_cast<uint32_t>(value)); };
            const std::array writes{
                buffer_write(descriptorSet(fi), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slices),
                image_write(descriptorSet(fi), 1),
                buffer_write(descriptorSet(fi), 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, config),
                buffer_write(
                    domain,
                    binding(ELightSetBindings::SHADOW_SLICES),
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    slices
                ),
                image_write(domain, binding(ELightSetBindings::SHADOW_ATLAS)),
                buffer_write(
                    domain,
                    binding(ELightSetBindings::SHADOW_CONFIG),
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    config
                ),
                buffer_write(
                    domain,
                    binding(ELightSetBindings::SHADOW_SPOT_MAP),
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    spot
                ),
                buffer_write(
                    domain,
                    binding(ELightSetBindings::SHADOW_POINT_MAP),
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    point
                )
            };
            vkUpdateDescriptorSets(
                device_.logicalDevice(),
                static_cast<uint32_t>(writes.size()),
                writes.data(),
                0,
                nullptr
            );
        }
    }

    Expected<void> ShadowResources::rebuild(uint32_t resolution, uint32_t pages, uint32_t slices) noexcept
    {
        const bool unchanged =
            resolution == atlasPageResolution() && pages == atlasPageCount() && slices == maxSlices();
        if (unchanged)
        {
            return {};
        }
        auto candidate = createBacking(device_, framesInFlight(), resolution, pages, slices);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        if (auto snapshot = findViewCache(debug_last_upload_.scene_key, debug_last_upload_.view_handle))
        {
            const auto count = std::min(snapshot->slices.size(), static_cast<size_t>(slices));
            auto config = snapshot->config;
            config.total_slices = std::min(config.total_slices, static_cast<uint32_t>(count));
            for (auto& frame : (*candidate)->frames)
            {
                if (count != 0)
                {
                    std::memcpy(frame.slices.mapped, snapshot->slices.data(), count * sizeof(ShadowSliceGPU));
                }
                std::memcpy(frame.config.mapped, &config, sizeof(config));
                for (const auto* buffer : {&frame.slices, &frame.config})
                {
                    const auto status =
                        vmaFlushAllocation(device_.vmaAllocator(), buffer->owner.allocation(), 0, VK_WHOLE_SIZE);
                    if (status != VK_SUCCESS)
                    {
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
                    }
                }
            }
        }
        const auto status = device_.waitIdle();
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        // Existing safety boundary: old graph cannot be using the accepted resources now.
        // Only complete candidates change descriptor contents or the cache lookup generation.
        backing_.swap(*candidate);
        writeDescriptors();
        per_view_cache_.clear();
        debug_last_upload_ = {};
        return {};
    }

    void ShadowResources::setCachedData(
        uint32_t scene_key,
        uint32_t view_handle,
        std::span<const ShadowSliceGPU> slices,
        std::span<const int32_t> spot_shadow_slice_index,
        std::span<const int32_t> point_shadow_base_slice,
        const ShadowConfigGPU& config,
        uint64_t frame_id,
        uint32_t frame_index
    )
    {
        // Build a FRESH immutable snapshot and swap it into the map.
        // Never mutate the existing entry in place: the cached render graph
        // REPLAYS its kernels, so a reader that captured the previous shared_ptr
        // can still be walking those vectors after this call — same thread,
        // later in the frame. Growing/reallocating them in place is a UAF.
        auto snapshot = std::make_shared<PerViewCache>();
        snapshot->slices.assign(slices.begin(), slices.end());
        snapshot->spot_shadow_slice_index.assign(spot_shadow_slice_index.begin(), spot_shadow_slice_index.end());
        snapshot->point_shadow_base_slice.assign(point_shadow_base_slice.begin(), point_shadow_base_slice.end());
        snapshot->config = config;

        per_view_cache_[makeViewCacheKey(scene_key, view_handle)] = std::move(snapshot);
        debug_last_upload_.scene_key = scene_key;
        debug_last_upload_.view_handle = view_handle;
        debug_last_upload_.frame_index = frame_index;
        debug_last_upload_.frame_id = frame_id;
        ++debug_last_upload_.sequence;
    }

    std::shared_ptr<const ShadowResources::PerViewCache> ShadowResources::findViewCache(
        uint32_t scene_key,
        uint32_t view_handle
    ) const noexcept
    {
        const auto it = per_view_cache_.find(makeViewCacheKey(scene_key, view_handle));
        if (it == per_view_cache_.end())
        {
            return nullptr;
        }
        return it->second; // shared_ptr copy: pins the snapshot for the caller
    }

    void ShadowResources::stampCacheFrame(
        uint32_t scene_key,
        uint32_t view_handle,
        uint64_t frame_id,
        uint32_t frame_index
    ) noexcept
    {
        // Pure diagnostic bookkeeping — does NOT touch the cached slice data.
        // Lets debugLastUploadSource() reflect the current frame even though
        // the actual write happened in onFrameBegin without a frame stamp.
        debug_last_upload_.scene_key = scene_key;
        debug_last_upload_.view_handle = view_handle;
        debug_last_upload_.frame_id = frame_id;
        debug_last_upload_.frame_index = frame_index;
        ++debug_last_upload_.sequence;
    }

    ShadowConfigGPU ShadowResources::config(uint32_t scene_key, uint32_t view_handle) const noexcept
    {
        if (auto cache = findViewCache(scene_key, view_handle))
        {
            return cache->config;
        }
        return default_config_;
    }

    void ShadowResources::evictSceneView(uint32_t scene_key, uint32_t view_id)
    {
        per_view_cache_.erase(makeViewCacheKey(scene_key, view_id));
        if (debug_last_upload_.scene_key == scene_key && debug_last_upload_.view_handle == view_id)
        {
            debug_last_upload_.scene_key = 0u;
            debug_last_upload_.view_handle = 0u;
        }
    }

} // namespace lux::render
