#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <vk_mem_alloc.h>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/core/LayoutTypes.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    void ShadowResources::init(const InitInfo &info)
    {
        device_ = info.device;
        device_context_ = info.device_context;
        allocator_ = info.allocator;
        atlas_page_resolution_ = std::max(info.atlas_page_resolution, 1u);
        atlas_page_count_ = std::max(info.atlas_page_count, 1u);
        max_shadow_slices_ = std::max(info.max_shadow_slices, 1u);
        frames_in_flight_ = info.frames_in_flight;
        shadow_ds_layout_id_ = info.ds_layout_id;
        descriptor_svc_ = info.descriptor_svc;
        arena_ = info.arena;
        light_resources_ = info.light_resources;

        // initialized_ goes true BEFORE the allocations, not after: shutdown()
        // early-outs on it, so a failure partway through would otherwise leave
        // the already-allocated atlas/SSBOs unreclaimable by anyone — the
        // destructor gates on the same flag. Set it first, and let the failure
        // path run the full shutdown().
        initialized_ = true;

        // 失败不在这里出声:render 层的诊断出口是 RenderErrorSink 的结构化错误,
        // 不是终端(no_terminal_io 门禁)。调用方 ShadowMapFeature::initAndAttachTo
        // 查 isInitialized() 并回 renderFailure<ResourceInitFailed>。
        if (!createShadowAtlas() || !createDescriptorResources())
            shutdown();   // reclaims whatever succeeded, and clears initialized_
    }

    void ShadowResources::shutdown()
    {
        if (!initialized_)
            return;

        vkDestroyImageView(device_, shadow_atlas_view_, nullptr);
        if (shadow_atlas_image_ != VK_NULL_HANDLE)
            vmaDestroyImage(allocator_, shadow_atlas_image_, shadow_atlas_alloc_);
        // shadow_sampler_ is deliberately absent: it comes from the
        // DescriptorService cache, which owns it until device teardown.

        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            if (slice_ssbos_[fi] != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, slice_ssbos_[fi], slice_ssbo_allocs_[fi]);
            if (config_ubos_[fi] != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, config_ubos_[fi], config_ubo_allocs_[fi]);
            if (spot_shadow_map_ssbos_[fi] != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, spot_shadow_map_ssbos_[fi], spot_shadow_map_ssbo_allocs_[fi]);
            if (point_shadow_map_ssbos_[fi] != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, point_shadow_map_ssbos_[fi], point_shadow_map_ssbo_allocs_[fi]);
        }

        shadow_atlas_view_ = VK_NULL_HANDLE;
        shadow_atlas_image_ = VK_NULL_HANDLE;
        shadow_sampler_ = VK_NULL_HANDLE;
        shadow_atlas_alloc_ = VK_NULL_HANDLE;
        slice_ssbos_.fill(VK_NULL_HANDLE);
        slice_ssbo_allocs_.fill(VK_NULL_HANDLE);
        slice_ssbo_mapped_.fill(nullptr);
        config_ubos_.fill(VK_NULL_HANDLE);
        config_ubo_allocs_.fill(VK_NULL_HANDLE);
        config_ubo_mapped_.fill(nullptr);
        spot_shadow_map_ssbos_.fill(VK_NULL_HANDLE);
        spot_shadow_map_ssbo_allocs_.fill(VK_NULL_HANDLE);
        spot_shadow_map_ssbo_mapped_.fill(nullptr);
        point_shadow_map_ssbos_.fill(VK_NULL_HANDLE);
        point_shadow_map_ssbo_allocs_.fill(VK_NULL_HANDLE);
        point_shadow_map_ssbo_mapped_.fill(nullptr);
        shadow_ds_per_fif_.clear();
        initialized_ = false;
        per_view_cache_.clear();
        debug_last_upload_ = {};
    }

    // =========================================================================
    // GPU resource creation
    // =========================================================================

    bool ShadowResources::createShadowAtlas()
    {
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = VK_FORMAT_D32_SFLOAT;
        img_ci.extent = {atlas_page_resolution_, atlas_page_resolution_, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = atlas_page_count_;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                           &shadow_atlas_image_, &shadow_atlas_alloc_, nullptr) != VK_SUCCESS)
        {
            shadow_atlas_image_ = VK_NULL_HANDLE;
            shadow_atlas_alloc_ = VK_NULL_HANDLE;
            return false;
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = shadow_atlas_image_;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_ci.format = VK_FORMAT_D32_SFLOAT;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_ci.subresourceRange.baseMipLevel = 0;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount = atlas_page_count_;
        if (vkCreateImageView(device_, &view_ci, nullptr, &shadow_atlas_view_) != VK_SUCCESS)
        {
            shadow_atlas_view_ = VK_NULL_HANDLE;
            return false;
        }

        // Shared cache, not a private VkSampler: the shape is expressible as a
        // SamplerDesc now that compare_op and border_color exist there. Owned by
        // DescriptorService for the device's lifetime, so this class must not
        // destroy it.
        shadow_sampler_ = descriptor_svc_->sampler(SamplerDesc::shadowCompare());
        if (shadow_sampler_ == VK_NULL_HANDLE)
        {
            return false;
        }
        return true;
    }

    bool ShadowResources::createDescriptorResources()
    {
        // Write shadow resources to Light descriptor sets at bindings 4-8
        if (!light_resources_)
        {
            return false;
        }

        const VkDeviceSize slice_buffer_size =
            static_cast<VkDeviceSize>(sizeof(ShadowSliceGPU)) * max_shadow_slices_;
        const VkDeviceSize map_buffer_size =
            static_cast<VkDeviceSize>(sizeof(int32_t)) * shadow_light_map_capacity_;

        // Create per-FIF Slice SSBO, Config UBO, and mapping SSBOs.
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            // Slice SSBO (persistently mapped)
            {
                VkBufferCreateInfo buf_ci{};
                buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                buf_ci.size = slice_buffer_size;
                buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                VmaAllocationCreateInfo alloc_ci{};
                alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo alloc_info{};
                if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                                    &slice_ssbos_[fi], &slice_ssbo_allocs_[fi], &alloc_info) != VK_SUCCESS)
                {
                    return false;
                }
                slice_ssbo_mapped_[fi] = alloc_info.pMappedData;
            }

            // Config UBO (persistently mapped)
            {
                VkBufferCreateInfo buf_ci{};
                buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                buf_ci.size = sizeof(ShadowConfigGPU);
                buf_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                VmaAllocationCreateInfo alloc_ci{};
                alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo alloc_info{};
                if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                                    &config_ubos_[fi], &config_ubo_allocs_[fi], &alloc_info) != VK_SUCCESS)
                {
                    return false;
                }
                config_ubo_mapped_[fi] = alloc_info.pMappedData;
            }

            // Spot shadow slot map SSBO (slot -> slice index)
            {
                VkBufferCreateInfo buf_ci{};
                buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                buf_ci.size = map_buffer_size;
                buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                VmaAllocationCreateInfo alloc_ci{};
                alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo alloc_info{};
                if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                                    &spot_shadow_map_ssbos_[fi], &spot_shadow_map_ssbo_allocs_[fi], &alloc_info) != VK_SUCCESS)
                {
                    return false;
                }
                spot_shadow_map_ssbo_mapped_[fi] = alloc_info.pMappedData;
            }

            // Point shadow slot map SSBO (slot -> base cube-face slice index)
            {
                VkBufferCreateInfo buf_ci{};
                buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                buf_ci.size = map_buffer_size;
                buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                VmaAllocationCreateInfo alloc_ci{};
                alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo alloc_info{};
                if (vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                                    &point_shadow_map_ssbos_[fi], &point_shadow_map_ssbo_allocs_[fi], &alloc_info) != VK_SUCCESS)
                {
                    return false;
                }
                point_shadow_map_ssbo_mapped_[fi] = alloc_info.pMappedData;
            }

            // Default mapping values to -1 (no shadow).
            std::vector<int32_t> init_map(shadow_light_map_capacity_, -1);
            std::memcpy(spot_shadow_map_ssbo_mapped_[fi], init_map.data(), static_cast<size_t>(map_buffer_size));
            std::memcpy(point_shadow_map_ssbo_mapped_[fi], init_map.data(), static_cast<size_t>(map_buffer_size));
            vmaFlushAllocation(allocator_, spot_shadow_map_ssbo_allocs_[fi], 0, map_buffer_size);
            vmaFlushAllocation(allocator_, point_shadow_map_ssbo_allocs_[fi], 0, map_buffer_size);
        }

        VkDescriptorImageInfo atlas_img_info{};
        atlas_img_info.sampler = shadow_sampler_;
        atlas_img_info.imageView = shadow_atlas_view_;
        atlas_img_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        shadow_ds_per_fif_.resize(frames_in_flight_, VK_NULL_HANDLE);
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            shadow_ds_per_fif_[fi] = arena_->allocate(descriptor_svc_->layout(shadow_ds_layout_id_));

            VkDescriptorBufferInfo slice_buf_info{};
            slice_buf_info.buffer = slice_ssbos_[fi];
            slice_buf_info.offset = 0;
            slice_buf_info.range = slice_buffer_size;

            VkDescriptorBufferInfo config_buf_info{};
            config_buf_info.buffer = config_ubos_[fi];
            config_buf_info.offset = 0;
            config_buf_info.range = sizeof(ShadowConfigGPU);

            std::array<VkWriteDescriptorSet, 3> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = shadow_ds_per_fif_[fi];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo = &slice_buf_info;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = shadow_ds_per_fif_[fi];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &atlas_img_info;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = shadow_ds_per_fif_[fi];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[2].pBufferInfo = &config_buf_info;

            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        writeShadowBindingsToLightAndDomain();
        return true;
    }

    void ShadowResources::writeShadowBindingsToLightAndDomain()
    {
        if (!light_resources_ || device_ == VK_NULL_HANDLE)
            return;

        const VkDeviceSize slice_buffer_size =
            static_cast<VkDeviceSize>(sizeof(ShadowSliceGPU)) * max_shadow_slices_;
        const VkDeviceSize map_buffer_size =
            static_cast<VkDeviceSize>(sizeof(int32_t)) * shadow_light_map_capacity_;

        VkDescriptorImageInfo atlas_img_info{};
        atlas_img_info.sampler = shadow_sampler_;
        atlas_img_info.imageView = shadow_atlas_view_;
        atlas_img_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        // Write per-FIF shadow bindings into per-FIF Light descriptor sets.
        // Each Light DS[fi] gets shadow buffers[fi] so that cross-FIF GPU overlap
        // cannot corrupt data read by DeferredLighting on a different FIF slot.
        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。循环改由
        // FIF 数驱动 —— 此前借 per-set 集的长度当计数,那是即将消失的东西。
        const uint32_t fif_count = light_resources_->framesInFlight();
        for (uint32_t fi = 0; fi < fif_count; ++fi)
        {
            VkDescriptorSet domain_ds =
                domain_.setFor(fi);
            if (domain_ds == VK_NULL_HANDLE)
                continue;

            VkDescriptorBufferInfo fi_slice_info{};
            fi_slice_info.buffer = slice_ssbos_[fi % frames_in_flight_];
            fi_slice_info.offset = 0;
            fi_slice_info.range = slice_buffer_size;

            VkDescriptorBufferInfo fi_config_info{};
            fi_config_info.buffer = config_ubos_[fi % frames_in_flight_];
            fi_config_info.offset = 0;
            fi_config_info.range = sizeof(ShadowConfigGPU);

            VkDescriptorBufferInfo fi_spot_map_info{};
            fi_spot_map_info.buffer = spot_shadow_map_ssbos_[fi % frames_in_flight_];
            fi_spot_map_info.offset = 0;
            fi_spot_map_info.range = map_buffer_size;

            VkDescriptorBufferInfo fi_point_map_info{};
            fi_point_map_info.buffer = point_shadow_map_ssbos_[fi % frames_in_flight_];
            fi_point_map_info.offset = 0;
            fi_point_map_info.range = map_buffer_size;

            // 这五条写的是 Light 集的 b4-b10(影子段),故 dstBinding 一律加
            // Light 域的域内偏移。本类自己的私有影子描述符集是 feature-owned、
            // 不参与域合并,不在这里处理。
            //
            // Light 集有两个所有者(b0-b3 归 LightResources,b4-b10 归本类)。
            // 合并进域集后这个"多所有者共享一个集"的模式不变 —— 只是从跨
            // 多个 binding 扩成跨多个 set。
            const auto db = [this](ELightSetBindings b) {
                return domain_.binding(static_cast<uint32_t>(b));
            };

            std::array<VkWriteDescriptorSet, 5> lw{};
            lw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            lw[0].dstSet = domain_ds;
            lw[0].dstBinding = db(ELightSetBindings::SHADOW_SLICES);
            lw[0].descriptorCount = 1;
            lw[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            lw[0].pBufferInfo = &fi_slice_info;
            lw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            lw[1].dstSet = domain_ds;
            lw[1].dstBinding = db(ELightSetBindings::SHADOW_ATLAS);
            lw[1].descriptorCount = 1;
            lw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            lw[1].pImageInfo = &atlas_img_info;
            lw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            lw[2].dstSet = domain_ds;
            lw[2].dstBinding = db(ELightSetBindings::SHADOW_CONFIG);
            lw[2].descriptorCount = 1;
            lw[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            lw[2].pBufferInfo = &fi_config_info;
            lw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            lw[3].dstSet = domain_ds;
            lw[3].dstBinding = db(ELightSetBindings::SHADOW_SPOT_MAP);
            lw[3].descriptorCount = 1;
            lw[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            lw[3].pBufferInfo = &fi_spot_map_info;
            lw[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            lw[4].dstSet = domain_ds;
            lw[4].dstBinding = db(ELightSetBindings::SHADOW_POINT_MAP);
            lw[4].descriptorCount = 1;
            lw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            lw[4].pBufferInfo = &fi_point_map_info;
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(lw.size()), lw.data(), 0, nullptr);
        }
    }

    Expected<void> ShadowResources::setDomainWriteTarget(std::span<const VkDescriptorSet> sets,
                                                         uint32_t binding_offset)
    {
        if (auto accepted = domain_.set(sets, binding_offset); !accepted)
            return accepted;
        // 必须在这里立刻重写一次:双写目标只在 init() 之后才可得(ShadowMapFeature 先
        // init()、再 setDomainWriteTarget()),只靠创建期那次写会让域副本的阴影段永久为空。
        // 而 PARTIALLY_BOUND 让空 binding 完全合法,于是光照从域副本读到 total_slices=0:
        // 阴影整体静默消失、光照本身照常工作、零 validation 错误。
        if (isInitialized())
            writeShadowBindingsToLightAndDomain();
        return {};
    }

    // =========================================================================
    // Rebuild helpers
    // =========================================================================

    bool ShadowResources::tryRebuild(
        uint32_t new_atlas_page_resolution,
        uint32_t new_atlas_page_count,
        uint32_t new_max_shadow_slices)
    {
        if (!initialized_)
            return false;

        new_atlas_page_resolution = std::max(new_atlas_page_resolution, 1u);
        new_atlas_page_count = std::max(new_atlas_page_count, 1u);
        new_max_shadow_slices = std::max(new_max_shadow_slices, 1u);

        if (new_atlas_page_resolution == atlas_page_resolution_
            && new_atlas_page_count == atlas_page_count_
            && new_max_shadow_slices == max_shadow_slices_)
        {
            return true; // no change needed
        }

        VkDevice saved_device = device_;
        VmaAllocator saved_alloc = allocator_;
        DescriptorLayoutId saved_layout_id = shadow_ds_layout_id_;
        DescriptorService *saved_ds_svc = descriptor_svc_;
        SceneDescriptorArena *saved_arena = arena_;
        LightResources *saved_light_res = light_resources_;
        uint32_t saved_fif = frames_in_flight_;

        std::vector<ShadowSliceGPU> saved_slices;
        ShadowConfigGPU saved_config{};
        uint32_t saved_slice_count = 0;
        if (auto last_view = findViewCache(
                debug_last_upload_.scene_key, debug_last_upload_.view_handle))
        {
            saved_slices = last_view->slices;
            saved_config = last_view->config;
            saved_slice_count = static_cast<uint32_t>(saved_slices.size());
        }

        if (device_context_ == nullptr ||
            device_context_->waitIdle() != VK_SUCCESS)
            return false;

        shutdown();

        InitInfo info{};
        info.device = saved_device;
        info.device_context = device_context_;
        info.allocator = saved_alloc;
        info.atlas_page_resolution = new_atlas_page_resolution;
        info.atlas_page_count = new_atlas_page_count;
        info.max_shadow_slices = new_max_shadow_slices;
        info.frames_in_flight = saved_fif;
        info.ds_layout_id = saved_layout_id;
        info.descriptor_svc = saved_ds_svc;
        info.arena = saved_arena;
        info.light_resources = saved_light_res;

        init(info);
        if (!initialized_)
            return false;

        if (saved_slice_count > 0 && !saved_slices.empty())
        {
            const uint32_t count = std::min(saved_slice_count, new_max_shadow_slices);
            flushSliceData({saved_slices.data(), count});
        }
        flushConfig(saved_config);
        return true;
    }

    // =========================================================================
    // Per-frame flush
    // =========================================================================

    void ShadowResources::flushSliceData(std::span<const ShadowSliceGPU> slices)
    {
        if (slices.empty())
            return;
        const size_t byte_size = slices.size() * sizeof(ShadowSliceGPU);
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            if (!slice_ssbo_mapped_[fi]) continue;
            std::memcpy(slice_ssbo_mapped_[fi], slices.data(), byte_size);
            vmaFlushAllocation(allocator_, slice_ssbo_allocs_[fi], 0, byte_size);
        }

    }

    void ShadowResources::flushConfig(const ShadowConfigGPU &config)
    {
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            if (!config_ubo_mapped_[fi]) continue;
            std::memcpy(config_ubo_mapped_[fi], &config, sizeof(ShadowConfigGPU));
            vmaFlushAllocation(allocator_, config_ubo_allocs_[fi], 0, sizeof(ShadowConfigGPU));
        }

    }

    void ShadowResources::setCachedData(
        uint32_t scene_key,
        uint32_t view_handle,
        std::span<const ShadowSliceGPU> slices,
        std::span<const int32_t> spot_shadow_slice_index,
        std::span<const int32_t> point_shadow_base_slice,
        const ShadowConfigGPU& config,
        uint64_t frame_id,
        uint32_t frame_index)
    {
        // Build a FRESH immutable snapshot and swap it into the map.
        // Never mutate the existing entry in place: the cached render graph
        // REPLAYS its kernels, so a reader that captured the previous shared_ptr
        // can still be walking those vectors after this call — same thread,
        // later in the frame. Growing/reallocating them in place is a UAF.
        auto snapshot = std::make_shared<PerViewCache>();
        snapshot->slices.assign(slices.begin(), slices.end());
        snapshot->spot_shadow_slice_index.assign(
            spot_shadow_slice_index.begin(), spot_shadow_slice_index.end());
        snapshot->point_shadow_base_slice.assign(
            point_shadow_base_slice.begin(), point_shadow_base_slice.end());
        snapshot->config = config;

        per_view_cache_[makeViewCacheKey(scene_key, view_handle)] = std::move(snapshot);
        debug_last_upload_.scene_key = scene_key;
        debug_last_upload_.view_handle = view_handle;
        debug_last_upload_.frame_index = frame_index;
        debug_last_upload_.frame_id = frame_id;
        ++debug_last_upload_.sequence;
    }

    std::shared_ptr<const ShadowResources::PerViewCache> ShadowResources::findViewCache(
        uint32_t scene_key, uint32_t view_handle) const noexcept
    {
        const auto it = per_view_cache_.find(makeViewCacheKey(scene_key, view_handle));
        if (it == per_view_cache_.end())
            return nullptr;
        return it->second;  // shared_ptr copy: pins the snapshot for the caller
    }

    void ShadowResources::stampCacheFrame(uint32_t scene_key, uint32_t view_handle,
                                          uint64_t frame_id, uint32_t frame_index) noexcept
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

    // (activeSliceCount 已删:零调用点。取切片数的现役路径是
    //  findViewCache(...)->slices.size(),消费方本就持着那份快照。)

    ShadowConfigGPU ShadowResources::config(uint32_t scene_key, uint32_t view_handle) const noexcept
    {
        if (auto cache = findViewCache(scene_key, view_handle))
            return cache->config;
        return default_config_;
    }

    // (这里曾有一个 onViewDestroyed 重写:按 view_id 跨全部 scene_key 逐出。
    //  它**改动前就已经是死代码** —— 注册表的通知路径始终走 onSceneViewDestroyed,
    //  而本类重写了后者,所以基类那条"onSceneViewDestroyed 默认转发到
    //  onViewDestroyed"的路径永远到不了这里。随虚接口一并删除。)

    void ShadowResources::evictSceneView(uint32_t scene_key, uint32_t view_id)
    {
        per_view_cache_.erase(makeViewCacheKey(scene_key, view_id));
        if (debug_last_upload_.scene_key == scene_key
            && debug_last_upload_.view_handle == view_id)
        {
            debug_last_upload_.scene_key = 0u;
            debug_last_upload_.view_handle = 0u;
        }
    }

} // namespace lux::render
