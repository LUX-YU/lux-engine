#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] bool isBlockCompressedVkFormat(VkFormat fmt) noexcept
        {
            switch (fmt)
            {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK:
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
            case VK_FORMAT_BC4_UNORM_BLOCK:
            case VK_FORMAT_BC4_SNORM_BLOCK:
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC5_SNORM_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
                return true;
            default:
                return false;
            }
        }

        /// Bytes per texel for the UNCOMPRESSED formats the persistent-texture /
        /// region-update path accepts (0 = not a supported format for that path).
        [[nodiscard]] uint32_t texelBytesOfVkFormat(VkFormat fmt) noexcept
        {
            switch (fmt)
            {
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_UNORM:      return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
            case VK_FORMAT_R8G8_UNORM:          return 2;
            case VK_FORMAT_R8_UNORM:            return 1;
            case VK_FORMAT_R16_UINT:            return 2;
            case VK_FORMAT_R16_UNORM:           return 2;
            default:                            return 0;
            }
        }
    }

    bool BindlessCombinedSet::init(const BCInitInfo &ci)
    {
        assert(ci.resource_context && ci.descriptor_set_layout);
        rc_ = ci.resource_context;

        set_index_ = ci.set_index;
        binding_ = ci.binding;
        set_layout_ = ci.descriptor_set_layout;

        queryFeaturesProps();
        null_desc_supported_ = robustness2_features_.nullDescriptor;

        layout_max_cap_ = clampLayoutMax(ci.layout_max_capacity);
        cur_cap_ = std::min(std::max(1u, ci.initial_capacity), layout_max_cap_);

        default_format_ = ci.default_image_format;
        srgb_for_color_ = ci.srgb_for_color;
        gen_mips_ = ci.generate_mipmaps;
        image_tiling_ = ci.image_tiling;
        image_usage_ = ci.image_usage;
        view_type_ = ci.view_type;
        image_aspect_ = ci.aspect;

        default_sampler_ci_ = ci.default_sampler_ci;
        if (default_sampler_ci_.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)
        {
            default_sampler_ci_ = {};
            default_sampler_ci_.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            default_sampler_ci_.magFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.minFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            default_sampler_ci_.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            default_sampler_ci_.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            default_sampler_ci_.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            default_sampler_ci_.maxLod = VK_LOD_CLAMP_NONE;
        }
        clear_on_remove_ = ci.clear_on_remove;

        // Pool / set allocation
        if (ci.external_pool && ci.external_set)
        {
            // External set mode: caller manages pool + set lifetime
            desc_pool_      = ci.external_pool;
            descriptor_set_ = ci.external_set;
            owns_pool_      = false;
            // In external mode, capacity is fixed at initial (no reallocation)
            cur_cap_ = layout_max_cap_;
        }
        else
        {
            buildPool();
            allocateSet(cur_cap_);
            owns_pool_ = true;
        }

        slots_.resize(layout_max_cap_);
        gen_.assign(layout_max_cap_, 1);
        alive_.assign(layout_max_cap_, 0);
        count_ = 0;

        frames_in_flight_ = std::max(1u, ci.frames_in_flight);
        deferred_staging_.resize(frames_in_flight_);

        // Create 1x1 fallback texture for writeCombinedDescriptorNull.
        // Always created because queryFeaturesProps() only checks physical
        // device *support* — the feature may not be *enabled* on the logical
        // device, making null descriptor writes invalid.
        {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType     = VK_IMAGE_TYPE_2D;
            ici.extent        = {1, 1, 1};
            ici.mipLevels     = 1;
            ici.arrayLayers   = 1;
            ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
            ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ici.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            VK_CHECK(vmaCreateImage(rc_->vmaAllocator(), &ici, &aci,
                                    &fallback_image_, &fallback_alloc_, nullptr));

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image    = fallback_image_;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(rc_->logicalDevice(), &vci, nullptr, &fallback_view_));

            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter    = VK_FILTER_NEAREST;
            sci.minFilter    = VK_FILTER_NEAREST;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &fallback_sampler_));

            // Transition to SHADER_READ_ONLY_OPTIMAL so descriptors referencing it are valid.
            auto cb_res = beginOneTime();
            if (cb_res)
            {
                VkCommandBuffer cb = cb_res.value();
                barrierImage(cb, fallback_image_, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                (void)endOneTime(cb);
            }
        }

        return true;
    }

    void BindlessCombinedSet::beginFrame()
    {
        // Auto-flush any pending texture uploads from previous frame
        flushUploads();
    }

    void BindlessCombinedSet::shutdown()
    {
        if (!rc_)
            return;

        // Clean up any pending uploads that were never flushed
        for (auto& p : pending_uploads_)
            destroyStaging(p.staging);
        pending_uploads_.clear();

        // Clean up deferred staging buffers from the last flush
        for (auto& slot : deferred_staging_)
        {
            for (auto& s : slot)
                destroyStaging(s);
            slot.clear();
        }
        deferred_staging_.clear();

        auto &device = rc_->logicalDevice();

        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (alive_[i])
                destroyCombined(slots_[i]);


        if (owns_pool_)
        {
            if (descriptor_set_ != VK_NULL_HANDLE)
            {
                vkFreeDescriptorSets(device, desc_pool_, 1, &descriptor_set_);
                descriptor_set_ = VK_NULL_HANDLE;
            }

            if (desc_pool_)
            {
                vkDestroyDescriptorPool(device, desc_pool_, nullptr);
                desc_pool_ = VK_NULL_HANDLE;
            }
        }
        else
        {
            // External mode: just clear our references
            descriptor_set_ = VK_NULL_HANDLE;
            desc_pool_ = VK_NULL_HANDLE;
        }
        set_layout_ = VK_NULL_HANDLE;

        // Destroy fallback texture resources
        if (fallback_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, fallback_view_, nullptr);
            fallback_view_ = VK_NULL_HANDLE;
        }
        if (fallback_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, fallback_sampler_, nullptr);
            fallback_sampler_ = VK_NULL_HANDLE;
        }
        if (fallback_image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(rc_->vmaAllocator(), fallback_image_, fallback_alloc_);
            fallback_image_ = VK_NULL_HANDLE;
            fallback_alloc_ = VK_NULL_HANDLE;
        }

        slots_.clear();
        gen_.clear();
        alive_.clear();
        free_.clear();
        rc_ = nullptr;
        count_ = cur_cap_ = layout_max_cap_ = 0;
    }

    SlotHandle BindlessCombinedSet::addTexture(
        const rdesc::Texture &tex,
        const VkSamplerCreateInfo *opt_sampler_ci,
        VkFormat fmt,
        std::optional<bool> gen_mips,
        std::optional<bool> srgb_override)
    {
        if (!ensureRoom())
            return SlotHandle{};
        const int w = tex.width(), h = tex.height(), c = tex.channel();
        assert(w > 0 && h > 0 && (c == 1 || c == 2 || c == 3 || c == 4));

        CombinedSlot s{};
        s.width = w;
        s.height = h;
        s.format = pickFormat(c, fmt, srgb_override.value_or(srgb_for_color_));

        TextureCopyPlan copy_plan{};
        const uint32_t provided_mips = std::clamp<uint32_t>(
            tex.mipCount(),
            1u,
            rdesc::kTextureMaxMipCount);
        const bool has_provided_mips = provided_mips > 1;
        const bool do_mips = gen_mips.value_or(gen_mips_)
                          && !isBlockCompressedVkFormat(s.format)
                          && !has_provided_mips;
        s.mip_levels = do_mips ? calcMipLevels(w, h) : provided_mips;

        if (has_provided_mips)
        {
            copy_plan.count = provided_mips;
            for (uint32_t i = 0; i < provided_mips; ++i)
            {
                const auto& mip = tex.mipRange(i);
                copy_plan.regions[i].buffer_offset = static_cast<VkDeviceSize>(mip.offset);
                copy_plan.regions[i].mip_level = i;
                copy_plan.regions[i].width = mip.width;
                copy_plan.regions[i].height = mip.height;
            }
        }
        else
        {
            copy_plan.count = 1;
            copy_plan.regions[0].buffer_offset = 0;
            copy_plan.regions[0].mip_level = 0;
            copy_plan.regions[0].width = static_cast<uint32_t>(w);
            copy_plan.regions[0].height = static_cast<uint32_t>(h);
        }

        // Create GPU image and staging buffer immediately
        createImageGPU(s);
        StagingBuf staging = createStaging(tex.size(), tex.data());

        // View + sampler can be created now (don't depend on image layout)
        createImageView(s);
        VkSamplerCreateInfo sci = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &s.sampler));

        // Assign slot and write descriptor (UPDATE_AFTER_BIND, valid before upload)
        uint32_t idx = allocIndex();
        slots_[idx] = s;
        alive_[idx] = 1;
        writeCombinedDescriptor(idx, s.view, s.sampler);

        // Defer GPU upload to batch (render-thread only; no lock needed)
        {
            pending_uploads_.push_back(PendingUpload{staging, idx, do_mips, false, 0, copy_plan});
        }
        return {idx, gen_[idx]};
    }

    SlotHandle BindlessCombinedSet::addPersistentTexture(
        uint32_t width, uint32_t height, uint32_t mip_levels, VkFormat fmt,
        const VkSamplerCreateInfo* opt_sampler_ci)
    {
        assert(view_type_ == VK_IMAGE_VIEW_TYPE_2D &&
               "addPersistentTexture() targets the 2D bindless set");
        if (width == 0 || height == 0 || mip_levels == 0 || fmt == VK_FORMAT_UNDEFINED)
            return SlotHandle{};
        if (!ensureRoom())
            return SlotHandle{};

        CombinedSlot s{};
        s.width        = static_cast<int>(width);
        s.height       = static_cast<int>(height);
        s.format       = fmt;
        s.mip_levels   = std::min<uint32_t>(mip_levels, calcMipLevels(width, height));
        s.array_layers = 1;
        createImageGPU(s);
        createImageView(s);
        VkSamplerCreateInfo sci = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &s.sampler));

        const uint32_t idx = allocIndex();
        slots_[idx] = s;
        alive_[idx] = 1;
        writeCombinedDescriptor(idx, s.view, s.sampler);

        // Zero-fill EVERY mip through the normal upload pipeline this frame: the whole
        // image lands in SHADER_READ_ONLY (so later region updates barrier from a
        // uniform known layout) and a sample before the first update reads zeros, not
        // undefined memory. texelBytesOfVkFormat is exact for the region-updatable
        // (uncompressed) formats this entry point accepts.
        const uint32_t texel = texelBytesOfVkFormat(fmt);
        assert(texel != 0 && "persistent textures must use an uncompressed format");
        TextureCopyPlan plan{};
        plan.count = std::min<uint32_t>(s.mip_levels, rdesc::kTextureMaxMipCount);
        VkDeviceSize total = 0;
        for (uint32_t m = 0; m < plan.count; ++m)
        {
            const uint32_t mw = std::max(1u, width  >> m);
            const uint32_t mh = std::max(1u, height >> m);
            plan.regions[m].buffer_offset = total;
            plan.regions[m].mip_level     = m;
            plan.regions[m].width         = mw;
            plan.regions[m].height        = mh;
            total += static_cast<VkDeviceSize>(mw) * mh * texel;
        }
        const std::vector<std::byte> zeros(static_cast<size_t>(total));   // value-init = 0
        StagingBuf staging = createStaging(total, zeros.data());
        pending_uploads_.push_back(PendingUpload{
            staging, idx, /*do_mips=*/false, /*is_cube=*/false, 0, plan,
            VK_IMAGE_LAYOUT_UNDEFINED,
        });
        return {idx, gen_[idx]};
    }

    bool BindlessCombinedSet::updateTextureRegions(
        const SlotHandle& h,
        std::span<const RegionUpdate> regions,
        std::span<const std::byte> pixels,
        uint32_t texel_bytes)
    {
        if (!isTextureAlive(h) || regions.empty() || texel_bytes == 0)
            return false;
        const uint32_t idx = h.index;

        // Ride the NORMAL pending-upload pipeline in ≤kTextureMaxMipCount-region
        // chunks (TextureCopyPlan's capacity): rows are repacked TIGHTLY into each
        // chunk's staging buffer, so VkBufferImageCopy never needs bufferRowLength
        // (and the wire row_pitch has no texel-alignment constraint).
        std::size_t next = 0;
        while (next < regions.size())
        {
            const uint32_t count = static_cast<uint32_t>(std::min<std::size_t>(
                regions.size() - next, rdesc::kTextureMaxMipCount));

            TextureCopyPlan plan{};
            plan.count = count;
            VkDeviceSize total = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const RegionUpdate& r = regions[next + i];
                plan.regions[i].buffer_offset = total;
                plan.regions[i].mip_level     = r.mip;
                plan.regions[i].width         = r.width;
                plan.regions[i].height        = r.height;
                plan.regions[i].x             = r.x;
                plan.regions[i].y             = r.y;
                plan.regions[i].array_layer   = r.array_layer;
                total += static_cast<VkDeviceSize>(r.width) * r.height * texel_bytes;
            }

            std::vector<std::byte> packed(static_cast<size_t>(total));
            VkDeviceSize out = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const RegionUpdate& r = regions[next + i];
                const std::size_t tight = static_cast<std::size_t>(r.width) * texel_bytes;
                const std::size_t pitch = r.row_pitch_bytes ? r.row_pitch_bytes : tight;
                const std::byte*  src   = pixels.data() + r.data_offset;
                for (uint32_t row = 0; row < r.height; ++row)
                {
                    std::memcpy(packed.data() + static_cast<size_t>(out),
                                src + static_cast<std::size_t>(row) * pitch, tight);
                    out += static_cast<VkDeviceSize>(tight);
                }
            }

            StagingBuf staging = createStaging(total, packed.data());
            pending_uploads_.push_back(PendingUpload{
                staging, idx, /*do_mips=*/false, /*is_cube=*/false, 0, plan,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   // persistent textures are always initialized
            });
            next += count;
        }
        return true;
    }

    SlotHandle BindlessCombinedSet::addCubeTexture(
        const rdesc::Texture faces[6],
        const VkSamplerCreateInfo *opt_sampler_ci,
        VkFormat fmt)
    {
        assert(view_type_ == VK_IMAGE_VIEW_TYPE_CUBE &&
               "addCubeTexture() requires a cube-view BindlessCombinedSet");
        if (!ensureRoom())
            return SlotHandle{};

        const int w = faces[0].width(), h = faces[0].height(), c = faces[0].channel();
        assert(w > 0 && h > 0 && w == h && (c == 1 || c == 2 || c == 3 || c == 4));

        CombinedSlot s{};
        s.width  = w;
        s.height = h;
        s.format = pickFormat(c, fmt);
        s.mip_levels   = 1;    // cubemap mipmaps not yet supported
        s.array_layers = 6;

        createImageGPU(s);

        // Combine all 6 faces into a single contiguous staging buffer
        const VkDeviceSize face_size  = faces[0].size();
        const VkDeviceSize total_size = face_size * 6;
        std::vector<uint8_t> combined(total_size);
        for (int i = 0; i < 6; ++i)
        {
            assert(faces[i].width() == w && faces[i].height() == h && faces[i].channel() == c);
            std::memcpy(combined.data() + i * face_size, faces[i].data(), face_size);
        }
        StagingBuf staging = createStaging(total_size, combined.data());

        createImageView(s);
        VkSamplerCreateInfo sci = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &s.sampler));

        uint32_t idx = allocIndex();
        slots_[idx] = s;
        alive_[idx] = 1;
        writeCombinedDescriptor(idx, s.view, s.sampler);

        {
            pending_uploads_.push_back(PendingUpload{staging, idx, false, true, face_size, {}});
        }
        return {idx, gen_[idx]};
    }

    void BindlessCombinedSet::flushUploads()
    {
        if (pending_uploads_.empty()) return;

        auto cb_result = beginOneTime();
        if (!cb_result) return;
        VkCommandBuffer cb = cb_result.value();
        recordPendingUploads(cb);
        auto end_result = endOneTime(cb);
        if (!end_result) return;
        // One-shot path: submission fence wait guarantees staging safety.
        for (auto& s : one_shot_staging_)
            destroyStaging(s);
        one_shot_staging_.clear();
    }

    void BindlessCombinedSet::flushUploads(VkCommandBuffer cb, uint32_t fi)
    {
        if (pending_uploads_.empty()) return;
        recordPendingUploads(cb);
        // Per-frame path: move collected staging into the frame-slot array.
        auto& slot = deferred_staging_[fi % frames_in_flight_];
        slot.insert(slot.end(),
                    std::make_move_iterator(one_shot_staging_.begin()),
                    std::make_move_iterator(one_shot_staging_.end()));
        one_shot_staging_.clear();
    }

    void BindlessCombinedSet::recordPendingUploads(VkCommandBuffer cb)
    {
        std::vector<PendingUpload> to_flush = std::move(pending_uploads_);

        for (auto& p : to_flush)
        {
            auto& s = slots_[p.slot_index];
            if (p.is_cube)
                recordCubeTextureUpload(cb, s, p.staging, p.face_stride, p.old_layout);
            else
                recordTextureUploadInternal(cb, s, p.staging, p.do_mips, &p.copy_plan, p.old_layout);
        }

        // Collect staging into temporary; caller decides where they go.
        for (auto& p : to_flush)
            one_shot_staging_.push_back(std::move(p.staging));
    }

    void BindlessCombinedSet::retireDeferredStaging(uint32_t fi)
    {
        auto& slot = deferred_staging_[fi % frames_in_flight_];
        for (auto& s : slot)
            destroyStaging(s);
        slot.clear();
    }

    // =========================================================================
    //  Transfer scheduler integration
    // =========================================================================

    void BindlessCombinedSet::submitTransfers(TransferScheduler& scheduler, uint32_t fi)
    {
        // ── Process pending_uploads_ (from addTexture / finalizeTexture) ─
        if (!pending_uploads_.empty())
        {
            std::vector<PendingUpload> to_flush = std::move(pending_uploads_);

            for (auto& p : to_flush)
            {
                auto& s = slots_[p.slot_index];

                if (p.is_cube)
                {
                    // Cube: one ImageCopyRequest per face
                    for (uint32_t face = 0; face < s.array_layers; ++face)
                    {
                        ImageCopyRequest req{};
                        req.src        = p.staging.buf;
                        req.src_offset = static_cast<VkDeviceSize>(face) * p.face_stride;
                        req.dst        = s.image;
                        req.old_layout = p.old_layout;
                        req.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = { image_aspect_, 0, face, 1 };
                        req.offset     = {0, 0, 0};
                        req.extent     = { static_cast<uint32_t>(s.width),
                                           static_cast<uint32_t>(s.height), 1 };
                        req.domain     = EBufferDomain::Sampled_FS;
                        scheduler.submitImageCopy(req);
                    }
                }
                else
                {
                    TextureCopyPlan fallback{};
                    const TextureCopyPlan* plan = &p.copy_plan;
                    if (plan->count == 0)
                    {
                        fallback.count = 1;
                        fallback.regions[0].buffer_offset = 0;
                        fallback.regions[0].mip_level     = 0;
                        fallback.regions[0].width  = static_cast<uint32_t>(std::max(1, s.width));
                        fallback.regions[0].height = static_cast<uint32_t>(std::max(1, s.height));
                        plan = &fallback;
                    }

                    // No mip clamp on count — see recordTextureUploadInternal:
                    // region updates carry many mip-0 rects; clamping dropped
                    // all but the first on 1-mip textures (stale-trail bug).
                    const uint32_t copy_count  = plan->count;
                    const bool runtime_mips    = p.do_mips && s.mip_levels > 1 && copy_count == 1;

                    for (uint32_t i = 0; i < copy_count; ++i)
                    {
                        const auto& cr = plan->regions[i];
                        ImageCopyRequest req{};
                        req.src        = p.staging.buf;
                        req.src_offset = cr.buffer_offset;
                        req.dst        = s.image;
                        req.old_layout = p.old_layout;
                        req.new_layout = runtime_mips
                                             ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = { image_aspect_,
                                            std::min(cr.mip_level, s.mip_levels - 1),
                                            cr.array_layer, 1 };
                        req.offset     = { static_cast<int32_t>(cr.x),
                                           static_cast<int32_t>(cr.y), 0 };
                        req.extent     = { std::max(1u, cr.width),
                                           std::max(1u, cr.height), 1 };
                        req.domain     = EBufferDomain::Sampled_FS;
                        scheduler.submitImageCopy(req);
                    }

                    if (runtime_mips)
                        pending_mip_gen_slots_.push_back(p.slot_index);
                }
            }

            // Move staging to deferred retirement for this frame slot.
            auto& def = deferred_staging_[fi % frames_in_flight_];
            for (auto& p : to_flush)
                def.push_back(std::move(p.staging));
        }

        // ── Process pending_staging_textures_ (iGPU StagingOnly fallback) ─
        if (!pending_staging_textures_.empty())
        {
            for (auto& e : pending_staging_textures_)
            {
                auto& s = slots_[e.slot_index];

                if (e.is_cube)
                {
                    for (uint32_t face = 0; face < s.array_layers; ++face)
                    {
                        ImageCopyRequest req{};
                        req.src        = e.stg_buf;
                        req.src_offset = static_cast<VkDeviceSize>(face) * e.face_stride;
                        req.dst        = s.image;
                        req.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                        req.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = { image_aspect_, 0, face, 1 };
                        req.offset     = {0, 0, 0};
                        req.extent     = { static_cast<uint32_t>(s.width),
                                           static_cast<uint32_t>(s.height), 1 };
                        req.domain     = EBufferDomain::Sampled_FS;
                        scheduler.submitImageCopy(req);
                    }
                }
                else
                {
                    TextureCopyPlan copy_plan{};
                    copy_plan.count = std::clamp<uint32_t>(
                        e.mip_copy_count, 1u, rdesc::kTextureMaxMipCount);
                    for (uint32_t i = 0; i < copy_plan.count; ++i)
                    {
                        copy_plan.regions[i].buffer_offset = e.mip_copies[i].buffer_offset;
                        copy_plan.regions[i].mip_level     = e.mip_copies[i].mip_level;
                        copy_plan.regions[i].width         = e.mip_copies[i].width;
                        copy_plan.regions[i].height        = e.mip_copies[i].height;
                    }

                    const uint32_t copy_count  = std::min(copy_plan.count, s.mip_levels);
                    const bool runtime_mips    = e.do_mips && s.mip_levels > 1 && copy_count == 1;

                    for (uint32_t i = 0; i < copy_count; ++i)
                    {
                        const auto& cr = copy_plan.regions[i];
                        ImageCopyRequest req{};
                        req.src        = e.stg_buf;
                        req.src_offset = cr.buffer_offset;
                        req.dst        = s.image;
                        req.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                        req.new_layout = runtime_mips
                                             ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = { image_aspect_,
                                            std::min(cr.mip_level, s.mip_levels - 1),
                                            cr.array_layer, 1 };
                        req.offset     = { static_cast<int32_t>(cr.x),
                                           static_cast<int32_t>(cr.y), 0 };
                        req.extent     = { std::max(1u, cr.width),
                                           std::max(1u, cr.height), 1 };
                        req.domain     = EBufferDomain::Sampled_FS;
                        scheduler.submitImageCopy(req);
                    }

                    if (runtime_mips)
                        pending_mip_gen_slots_.push_back(e.slot_index);
                }
            }
            pending_staging_textures_.clear();
        }

        // ── Convert pending acquire barriers to QFOT requests ────────
        if (!pending_acquire_barriers_.empty())
        {
            for (const auto& b : pending_acquire_barriers_)
            {
                QFOTAcquireRequest req{};
                req.kind       = QFOTAcquireRequest::Kind::Image;
                req.image      = b.image;
                req.img_layout = b.oldLayout;
                req.img_range  = b.subresourceRange;
                req.src_family = b.srcQueueFamilyIndex;
                req.dst_family = b.dstQueueFamilyIndex;
                req.domain     = EBufferDomain::Sampled_FS;
                scheduler.submitQFOTAcquire(req);
            }
            pending_acquire_barriers_.clear();
        }
    }

    void BindlessCombinedSet::postTransfer(VkCommandBuffer cmd)
    {
        recordDeferredMipGens(cmd);
    }

    bool BindlessCombinedSet::removeTexture(const SlotHandle &h)
    {
        if (!isTextureAlive(h))
            return false;
        const uint32_t idx = h.index;

        // CRITICAL: frames N-1/N-2 in flight may still sample this slot's view.
        // Retire the GPU objects through the deferred-destroy queue and hold the
        // slot index out of the free list until the same retire serial is GPU-
        // complete (recycleCompletedSlots). The descriptor still points at the
        // (deferred, still-alive) view during that window; the index is only
        // reused — and its descriptor rewritten — after no pending frame can
        // reference it, which is exactly when UPDATE_AFTER_BIND permits the
        // rewrite. Bump the generation now so all outstanding handles go stale.
        retireCombinedDeferred(slots_[idx]);
        slots_[idx] = {};
        alive_[idx] = 0;
        gen_[idx]++;

        if (deferred_queue_)
            pending_recycle_.emplace_back(idx, deferred_queue_->currentSerial());
        else
            free_.push_back(idx);  // standalone/tests (waitIdle): immediate reuse is safe

        return true;
    }

    // =========================================================================
    // A1: Render-thread slot finalize + recycle API
    // =========================================================================

    void BindlessCombinedSet::finalizeTexture(
        SlotHandle                  h,
        const uint8_t*              pixels,
        std::size_t                 size,
        int32_t                     w,
        int32_t                     h_dim,
        int32_t                     c,
        VkFormat                    fmt_hint,
        const VkSamplerCreateInfo&  sci,
        bool                        do_mips,
        bool                        srgb)
    {
        assert(rc_ && "BindlessCombinedSet not initialised");
        assert(h.valid() && h.index < layout_max_cap_);

        const uint32_t idx = h.index;

        CombinedSlot s{};
        s.width       = w;
        s.height      = h_dim;
        s.format      = pickFormat(c, fmt_hint, srgb);
        const bool effective_mips = do_mips && !isBlockCompressedVkFormat(s.format);
        s.mip_levels  = effective_mips ? calcMipLevels(static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h_dim)) : 1u;
        s.array_layers = 1;

        createImageGPU(s);
        createImageView(s);
        VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &s.sampler));

        slots_[idx] = s;
        alive_[idx] = 1;
        writeCombinedDescriptor(idx, s.view, s.sampler);  // UPDATE_AFTER_BIND

        StagingBuf staging = createStaging(static_cast<VkDeviceSize>(size), pixels);
        TextureCopyPlan copy_plan{};
        copy_plan.count = 1;
        copy_plan.regions[0].buffer_offset = 0;
        copy_plan.regions[0].mip_level = 0;
        copy_plan.regions[0].width = static_cast<uint32_t>(std::max(1, w));
        copy_plan.regions[0].height = static_cast<uint32_t>(std::max(1, h_dim));
        pending_uploads_.push_back(PendingUpload{staging, idx, effective_mips, false, 0, copy_plan});
    }

    void BindlessCombinedSet::finalizeCubeTexture(
        SlotHandle                  h,
        const uint8_t*              combined_pixels,
        std::size_t                 total_size,
        int32_t                     w,
        int32_t                     h_dim,
        int32_t                     c,
        VkFormat                    fmt_hint,
        const VkSamplerCreateInfo&  sci,
        VkDeviceSize                face_stride)
    {
        assert(rc_ && "BindlessCombinedSet not initialised");
        assert(h.valid() && h.index < layout_max_cap_);
        assert(view_type_ == VK_IMAGE_VIEW_TYPE_CUBE);

        const uint32_t idx = h.index;

        CombinedSlot s{};
        s.width        = w;
        s.height       = h_dim;
        s.format       = pickFormat(c, fmt_hint, false);
        s.mip_levels   = 1;   // no auto mips for cube maps
        s.array_layers = 6;

        createImageGPU(s);
        createImageView(s);
        VK_CHECK(vkCreateSampler(rc_->logicalDevice(), &sci, nullptr, &s.sampler));

        slots_[idx] = s;
        alive_[idx] = 1;
        writeCombinedDescriptor(idx, s.view, s.sampler);

        StagingBuf staging = createStaging(static_cast<VkDeviceSize>(total_size), combined_pixels);
        pending_uploads_.push_back(PendingUpload{staging, idx, false, true, face_stride, {}});
    }

    bool BindlessCombinedSet::updateTextureMips(
        const SlotHandle& h,
        std::span<const TextureUpdateMip> mips,
        bool generate_mips)
    {
        if (!isTextureAlive(h) || mips.empty())
            return false;

        const uint32_t idx = h.index;
        CombinedSlot& s = slots_[idx];
        if (s.array_layers != 1)
            return false;

        const uint32_t copy_count = std::clamp<uint32_t>(
            static_cast<uint32_t>(mips.size()),
            1u,
            rdesc::kTextureMaxMipCount);
        if (copy_count > s.mip_levels)
            return false;

        const auto& base = mips[0];
        if (base.width == 0 || base.height == 0)
            return false;
        if (static_cast<int32_t>(base.width) != s.width ||
            static_cast<int32_t>(base.height) != s.height)
            return false;

        const bool do_runtime_mips = generate_mips
                                  && copy_count == 1
                                  && s.mip_levels > 1
                                  && !isBlockCompressedVkFormat(s.format);

        TextureCopyPlan copy_plan{};
        copy_plan.count = copy_count;

        VkDeviceSize total_bytes = 0;
        for (uint32_t i = 0; i < copy_count; ++i)
        {
            const auto& mip = mips[i];
            if (mip.data == nullptr || mip.bytes == 0 || mip.width == 0 || mip.height == 0)
                return false;

            copy_plan.regions[i].buffer_offset = total_bytes;
            copy_plan.regions[i].mip_level = i;
            copy_plan.regions[i].width = mip.width;
            copy_plan.regions[i].height = mip.height;
            total_bytes += static_cast<VkDeviceSize>(mip.bytes);
        }

        std::vector<std::byte> packed;
        packed.resize(static_cast<size_t>(total_bytes));

        VkDeviceSize offset = 0;
        for (uint32_t i = 0; i < copy_count; ++i)
        {
            const auto& mip = mips[i];
            std::memcpy(
                packed.data() + static_cast<size_t>(offset),
                mip.data,
                mip.bytes);
            offset += static_cast<VkDeviceSize>(mip.bytes);
        }

        StagingBuf staging = createStaging(total_bytes, packed.data());
        pending_uploads_.push_back(PendingUpload{
            std::move(staging),
            idx,
            do_runtime_mips,
            false,
            0,
            copy_plan,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        return true;
    }

    bool BindlessCombinedSet::updateCubeFaces(
        const SlotHandle& h,
        const std::array<TextureUpdateFace, 6>& faces)
    {
        if (!isTextureAlive(h))
            return false;

        const uint32_t idx = h.index;
        CombinedSlot& s = slots_[idx];
        if (s.array_layers != 6)
            return false;

        VkDeviceSize face_stride = 0;
        for (const auto& face : faces)
        {
            if (face.data == nullptr || face.bytes == 0)
                return false;
            if (face_stride == 0)
                face_stride = static_cast<VkDeviceSize>(face.bytes);
            else if (face_stride != static_cast<VkDeviceSize>(face.bytes))
                return false;
        }

        const VkDeviceSize total_bytes = face_stride * 6;
        std::vector<std::byte> packed;
        packed.resize(static_cast<size_t>(total_bytes));

        VkDeviceSize offset = 0;
        for (const auto& face : faces)
        {
            std::memcpy(
                packed.data() + static_cast<size_t>(offset),
                face.data,
                face.bytes);
            offset += static_cast<VkDeviceSize>(face.bytes);
        }

        StagingBuf staging = createStaging(total_bytes, packed.data());
        pending_uploads_.push_back(PendingUpload{
            std::move(staging),
            idx,
            false,
            true,
            face_stride,
            {},
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        return true;
    }

    void BindlessCombinedSet::destroySlotForRecycle(SlotHandle h) noexcept
    {
        if (!isTextureAlive(h)) return;

        const uint32_t idx = h.index;
        destroyCombined(slots_[idx]);
        slots_[idx] = {};
        alive_[idx] = 0;
        // Increment generation BEFORE the caller pushes idx to the SPSC recycled
        // queue; the SPSC push is the release fence that makes this write visible
        // to the game thread after it pops the index.
        gen_[idx]++;

        if (clear_on_remove_ && null_desc_supported_)
            writeCombinedDescriptorNull(idx);
    }

    // ---------- allocateSlotDeferred ----------
    SlotHandle BindlessCombinedSet::allocateSlotDeferred()
    {
        if (!ensureRoom())
            return SlotHandle{};
        uint32_t idx = allocIndex();
        alive_[idx] = 1;
        // Write null / fallback descriptor so shaders see valid texels.
        writeCombinedDescriptorNull(idx);
        return { idx, gen_[idx] };
    }

    // ---------- finalizeTransferredTexture ----------
    void BindlessCombinedSet::finalizeTransferredTexture(
        uint32_t      slot_idx,
        VkImage       image,
        VmaAllocation alloc,
        VkImageView   view,
        VkSampler     sampler,
        VkFormat      format,
        uint32_t      mip_levels,
        uint32_t      array_layers,
        int32_t       w,
        int32_t       h)
    {
        CombinedSlot& s = slots_[slot_idx];
        s.image        = image;
        s.alloc        = alloc;
        s.view         = view;
        s.sampler      = sampler;
        s.format       = format;
        s.mip_levels   = mip_levels;
        s.array_layers = array_layers;
        s.width        = w;
        s.height       = h;
        writeCombinedDescriptor(slot_idx, view, sampler);
    }

    void BindlessCombinedSet::reserve(uint32_t need)
    {
        if (need <= cur_cap_) return;
        uint32_t new_cap = std::min(nextCap(cur_cap_, need), layout_max_cap_);

        assert(new_cap <= layout_max_cap_ && "Need to rebuild layout with larger descriptorCount");
        reallocateSetAndCopy(new_cap);
        if (slots_.size() < new_cap)
            slots_.resize(new_cap);
        if (gen_.size() < new_cap)
            gen_.resize(new_cap, 1);
        if (alive_.size() < new_cap)
            alive_.resize(new_cap, 0);
        cur_cap_ = new_cap;
    }

    // ===== Private methods =====

    void BindlessCombinedSet::queryFeaturesProps()
    {
        features_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        robustness2_features_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        features_.pNext = &robustness2_features_;

        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features_};
        vkGetPhysicalDeviceFeatures2(rc_->physicalDevice(), &f2);

        indexing_props_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing_props_};
        vkGetPhysicalDeviceProperties2(rc_->physicalDevice(), &p2);
    }

    uint32_t BindlessCombinedSet::clampLayoutMax(uint32_t want) const
    {
        auto& phys = rc_->physicalDevice();
        auto& idx_properties = phys.descriptorIndexingProperties();

        uint32_t m = want;
        m = std::min(m, idx_properties.maxDescriptorSetUpdateAfterBindSampledImages);
        m = std::min(m, idx_properties.maxPerStageDescriptorUpdateAfterBindSampledImages);
        m = std::min(m, idx_properties.maxPerStageUpdateAfterBindResources);
        return std::max(1u, m);
    }

    void BindlessCombinedSet::buildPool()
    {
        auto &device = rc_->logicalDevice();

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, layout_max_cap_ * (kMaxFramesInFlight + 2u)};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = kMaxFramesInFlight + 2;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        VK_CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &desc_pool_));
    }

    void BindlessCombinedSet::allocateSet(uint32_t varCount)
    {
        auto &device = rc_->logicalDevice();

        VkDescriptorSetVariableDescriptorCountAllocateInfo vci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vci.descriptorSetCount = 1;
        vci.pDescriptorCounts = &varCount;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        ai.pNext = &vci;

        VK_CHECK(vkAllocateDescriptorSets(device, &ai, &descriptor_set_));
    }

    void BindlessCombinedSet::reallocateSetAndCopy(uint32_t newCount)
    {
        auto &device = rc_->logicalDevice();

        VkDescriptorSet newSet = VK_NULL_HANDLE;
        {
            VkDescriptorSetVariableDescriptorCountAllocateInfo vci{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
            vci.descriptorSetCount = 1;
            vci.pDescriptorCounts = &newCount;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = desc_pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layout_;
            ai.pNext = &vci;
            VK_CHECK(vkAllocateDescriptorSets(device, &ai, &newSet));
        }

        std::vector<VkCopyDescriptorSet> copies;
        copies.reserve(count_);
        for (uint32_t i = 0; i < count_; ++i)
        {
            if (i < alive_.size() && alive_[i] && slots_[i].view)
            {
                VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
                c.srcSet = descriptor_set_;
                c.srcBinding = binding_;
                c.srcArrayElement = i;
                c.dstSet = newSet;
                c.dstBinding = binding_;
                c.dstArrayElement = i;
                c.descriptorCount = 1;
                copies.push_back(c);
            }
        }
        if (!copies.empty())
            vkUpdateDescriptorSets(device, 0, nullptr, (uint32_t)copies.size(), copies.data());

        if (descriptor_set_ != VK_NULL_HANDLE)
        {
            if (deferred_queue_ != nullptr)
            {
                deferred_queue_->retireDescriptorSet(desc_pool_, descriptor_set_);
            }
            else
            {
                // owns-pool growth requires a deferred-destroy queue (setDeferredQueue)
                // so the superseded set outlives in-flight frames. deferred_queue_
                // defaults to null and only this owns-pool reserve() path reaches
                // here, so a missing queue is a setup error, not a runtime condition.
                // Don't dereference null: assert in debug, and fall back to a hard
                // device-idle + synchronous free (the pool is freeable — the deferred
                // path frees the same way). (M7)
                assert(false && "BindlessCombinedSet owns-pool growth needs setDeferredQueue()");
                vkDeviceWaitIdle(device);
                vkFreeDescriptorSets(device, desc_pool_, 1, &descriptor_set_);
            }
        }
        descriptor_set_ = newSet;
    }


    bool BindlessCombinedSet::ensureRoom()
    {
        if (!free_.empty())
            return true;
        if (count_ < cur_cap_)
            return true;
        if (!owns_pool_) {
            // External pool mode: cannot reallocate. Caller must handle.
            return false;
        }
        reserve(count_ + 1);
        return true;
    }

    void BindlessCombinedSet::writeCombinedDescriptor(uint32_t idx, VkImageView view, VkSampler sampler)
    {
        VkDescriptorImageInfo di{};
        di.imageView = view;
        di.sampler = sampler;
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descriptor_set_;
        w.dstBinding = binding_;
        w.dstArrayElement = idx;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &di;
        vkUpdateDescriptorSets(rc_->logicalDevice(), 1, &w, 0, nullptr);
    }

    void BindlessCombinedSet::writeCombinedDescriptorNull(uint32_t idx)
    {
        VkDescriptorImageInfo di{};
        di.sampler     = fallback_sampler_;
        di.imageView   = fallback_view_;
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descriptor_set_;
        w.dstBinding = binding_;
        w.dstArrayElement = idx;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &di;
        vkUpdateDescriptorSets(rc_->logicalDevice(), 1, &w, 0, nullptr);
    }

    void BindlessCombinedSet::createImageGPU(CombinedSlot &s)
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.extent = {(uint32_t)s.width, (uint32_t)s.height, 1u};
        ici.mipLevels = s.mip_levels;
        ici.arrayLayers = s.array_layers;
        ici.format = s.format;
        ici.tiling = image_tiling_;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.usage = image_usage_;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (s.array_layers == 6)
            ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VK_CHECK(vmaCreateImage(rc_->vmaAllocator(), &ici, &aci, &s.image, &s.alloc, nullptr));
    }

    void BindlessCombinedSet::createImageView(CombinedSlot &s)
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s.image;
        vci.viewType = view_type_;
        vci.format = s.format;
        vci.subresourceRange = {image_aspect_, 0, s.mip_levels, 0, s.array_layers};
        VK_CHECK(vkCreateImageView(rc_->logicalDevice(), &vci, nullptr, &s.view));
    }

    void BindlessCombinedSet::destroyCombined(CombinedSlot &s)
    {
        auto &dev = rc_->logicalDevice();
        if (s.sampler)
        {
            vkDestroySampler(dev, s.sampler, nullptr);
            s.sampler = VK_NULL_HANDLE;
        }
        if (s.view)
        {
            vkDestroyImageView(dev, s.view, nullptr);
            s.view = VK_NULL_HANDLE;
        }
        if (s.image)
        {
            vmaDestroyImage(rc_->vmaAllocator(), s.image, s.alloc);
            s.image = VK_NULL_HANDLE;
            s.alloc = VK_NULL_HANDLE;
        }
    }

    void BindlessCombinedSet::retireCombinedDeferred(CombinedSlot &s)
    {
        if (!deferred_queue_)
        {
            // Standalone / tests with no destroy queue: callers waitIdle before
            // teardown, so immediate destruction is safe there.
            destroyCombined(s);
            return;
        }
        if (s.sampler)
            deferred_queue_->retireSampler(s.sampler);
        if (s.view)
            deferred_queue_->retireImageView(s.view);
        if (s.image)
            deferred_queue_->retireImage(s.image, s.alloc);
        s.sampler = VK_NULL_HANDLE;
        s.view    = VK_NULL_HANDLE;
        s.image   = VK_NULL_HANDLE;
        s.alloc   = VK_NULL_HANDLE;
    }

    void BindlessCombinedSet::recycleCompletedSlots(uint64_t completed_serial)
    {
        // Move indices whose removeTexture() retire-serial is now GPU-complete
        // back into the free list. Stable partition: keep the still-pending tail.
        std::size_t keep = 0;
        for (std::size_t i = 0; i < pending_recycle_.size(); ++i)
        {
            if (pending_recycle_[i].second <= completed_serial)
                free_.push_back(pending_recycle_[i].first);
            else
                pending_recycle_[keep++] = pending_recycle_[i];
        }
        pending_recycle_.resize(keep);
    }

    BindlessCombinedSet::StagingBuf BindlessCombinedSet::createStaging(VkDeviceSize size, const void *data)
    {
        StagingBuf b{};
        b.size = size;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocationInfo ai{};
        VK_CHECK(vmaCreateBuffer(rc_->vmaAllocator(), &bci, &aci, &b.buf, &b.alloc, &ai));
        std::memcpy(ai.pMappedData, data, (size_t)size);
        return b;
    }

    void BindlessCombinedSet::destroyStaging(StagingBuf &b)
    {
        if (b.buf)
            vmaDestroyBuffer(rc_->vmaAllocator(), b.buf, b.alloc);
        b = {};
    }

    Expected<VkCommandBuffer> BindlessCombinedSet::beginOneTime()
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = rc_->commandPool();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb{};
        VK_EXPECT(vkAllocateCommandBuffers(rc_->logicalDevice(), &ai, &cb));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_EXPECT(vkBeginCommandBuffer(cb, &bi));
        return cb;
    }

    Expected<void> BindlessCombinedSet::endOneTime(VkCommandBuffer cb)
    {
        VK_EXPECT(vkEndCommandBuffer(cb));
        VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        VK_EXPECT(vkCreateFence(rc_->logicalDevice(), &fence_ci, nullptr, &fence));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VkResult submit_res = vkQueueSubmit(rc_->graphicsQueue(), 1, &si, fence);
        if (submit_res != VK_SUCCESS)
        {
            vkDestroyFence(rc_->logicalDevice(), fence, nullptr);
            return lux::cxx::unexpected(
                make_error_code(ERenderError::InternalError));
        }
        VkResult wait_res = vkWaitForFences(rc_->logicalDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(rc_->logicalDevice(), fence, nullptr);
        if (wait_res != VK_SUCCESS)
        {
            return lux::cxx::unexpected(
                make_error_code(ERenderError::InternalError));
        }
        vkFreeCommandBuffers(rc_->logicalDevice(), rc_->commandPool(), 1, &cb);
        return {};
    }

    void BindlessCombinedSet::barrierImage(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                             VkImageLayout oldL, VkImageLayout newL,
                             uint32_t base, uint32_t levels, uint32_t layer_count)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.image = img;
        b.subresourceRange = {aspect, base, levels, 0, layer_count};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
    }

    void BindlessCombinedSet::genMipsLinear(
        VkCommandBuffer cb,
        CombinedSlot &s,
        VkImageLayout untouched_old_layout)
    {
        int32_t w = s.width, h = s.height;
        for (uint32_t i = 1; i < s.mip_levels; ++i)
        {
            barrierImage(cb, s.image, image_aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i - 1, 1);
            int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);

            VkImageBlit bl{};
            bl.srcSubresource = {image_aspect_, i - 1, 0, 1};
            bl.srcOffsets[0] = {0, 0, 0};
            bl.srcOffsets[1] = {w, h, 1};
            bl.dstSubresource = {image_aspect_, i, 0, 1};
            bl.dstOffsets[0] = {0, 0, 0};
            bl.dstOffsets[1] = {nw, nh, 1};

            barrierImage(cb, s.image, image_aspect_, untouched_old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, i, 1);
            vkCmdBlitImage(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);

            w = nw;
            h = nh;
        }
        
        if (s.mip_levels > 1) {
            barrierImage(cb, s.image, image_aspect_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, s.mip_levels - 1);
        }
        barrierImage(cb, s.image, image_aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.mip_levels - 1, 1);
    }

    void BindlessCombinedSet::recordTextureUploadInternal(VkCommandBuffer cb, BindlessCombinedSet::CombinedSlot &s,
                                                            BindlessCombinedSet::StagingBuf &staging, bool do_mips,
                                                            const BindlessCombinedSet::TextureCopyPlan* copy_plan,
                                                            VkImageLayout old_layout)
    {
        TextureCopyPlan fallback{};
        if (!copy_plan || copy_plan->count == 0)
        {
            fallback.count = 1;
            fallback.regions[0].buffer_offset = 0;
            fallback.regions[0].mip_level = 0;
            fallback.regions[0].width = static_cast<uint32_t>(std::max(1, s.width));
            fallback.regions[0].height = static_cast<uint32_t>(std::max(1, s.height));
            copy_plan = &fallback;
        }

        // NOTE: copy_plan->count is NOT clamped to s.mip_levels — mip-chain
        // uploads carry one region per level (count == mips by construction),
        // but REGION UPDATES carry arbitrary many mip-0 rects; clamping used
        // to silently drop every region after the first on a 1-mip texture
        // (the 2026-07-10 stale-pixel-trail bug — the ack still said Ok, so
        // the planner retired dirt that never reached the GPU). The per-region
        // mip index below is still clamped.
        const uint32_t copy_count = copy_plan->count;
        const bool runtime_mips = do_mips && s.mip_levels > 1 && copy_count == 1;
        const uint32_t transition_levels = runtime_mips ? 1u : s.mip_levels;

        barrierImage(cb, s.image, image_aspect_, old_layout,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, transition_levels);

        std::vector<VkBufferImageCopy> copies;
        copies.reserve(copy_count);
        for (uint32_t i = 0; i < copy_count; ++i)
        {
            const auto& cr = copy_plan->regions[i];
            VkBufferImageCopy r{};
            r.bufferOffset = cr.buffer_offset;
            r.imageSubresource = {
                image_aspect_,
                std::min(cr.mip_level, s.mip_levels - 1),
                cr.array_layer,   // 0 for full-mip uploads; region updates target a layer
                1
            };
            r.imageOffset = {
                static_cast<int32_t>(cr.x),   // 0 for full-mip uploads
                static_cast<int32_t>(cr.y),
                0
            };
            r.imageExtent = {
                std::max(1u, cr.width),
                std::max(1u, cr.height),
                1
            };
            copies.push_back(r);
        }
        vkCmdCopyBufferToImage(cb, staging.buf, s.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copies.size()),
                               copies.data());

        if (runtime_mips)
            genMipsLinear(cb, s, old_layout);
        else
            barrierImage(cb, s.image, image_aspect_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, s.mip_levels);
    }

    void BindlessCombinedSet::recordCubeTextureUpload(VkCommandBuffer cb, CombinedSlot &s,
                                                       StagingBuf &staging, VkDeviceSize face_stride,
                                                       VkImageLayout old_layout)
    {
        // Transition all 6 layers to TRANSFER_DST
        barrierImage(cb, s.image, image_aspect_,
                     old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, s.mip_levels, s.array_layers);

        // Copy 6 faces from staging buffer (contiguous, one per layer)
        std::array<VkBufferImageCopy, 6> regions{};
        for (uint32_t face = 0; face < 6; ++face)
        {
            regions[face].bufferOffset        = face * face_stride;
            regions[face].imageSubresource    = {image_aspect_, 0, face, 1};
            regions[face].imageExtent         = {(uint32_t)s.width, (uint32_t)s.height, 1};
        }
        vkCmdCopyBufferToImage(cb, staging.buf, s.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               6, regions.data());

        // Transition to SHADER_READ_ONLY
        barrierImage(cb, s.image, image_aspect_,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0, s.mip_levels, s.array_layers);
    }

    // ---------- Async image acquire barriers ----------

    void BindlessCombinedSet::pushImageAcquireBarrier(
        VkImage      image,
        uint32_t     mip_levels,
        uint32_t     array_layers,
        VkImageLayout layout,
        uint32_t     src_queue_family,
        uint32_t     dst_queue_family)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask       = VK_ACCESS_2_NONE;
        b.dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout           = layout;
        b.newLayout           = layout;
        b.srcQueueFamilyIndex = src_queue_family;
        b.dstQueueFamilyIndex = dst_queue_family;
        b.image               = image;
        b.subresourceRange    = {image_aspect_, 0, mip_levels, 0, array_layers};
        pending_acquire_barriers_.push_back(b);
    }

    void BindlessCombinedSet::recordAcquireBarriers(VkCommandBuffer cmd)
    {
        if (pending_acquire_barriers_.empty()) return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(pending_acquire_barriers_.size());
        dep.pImageMemoryBarriers    = pending_acquire_barriers_.data();
        vkCmdPipelineBarrier2(cmd, &dep);
        pending_acquire_barriers_.clear();
    }

    // ---------- StagingOnly texture copy fallback ----------

    void BindlessCombinedSet::pushStagingTextureCopy(const PendingStagingTexture& entry)
    {
        pending_staging_textures_.push_back(entry);
    }

    void BindlessCombinedSet::recordStagingTextureCopies(VkCommandBuffer cmd)
    {
        if (pending_staging_textures_.empty()) return;

        for (auto& e : pending_staging_textures_)
        {
            auto& s = slots_[e.slot_index];
            StagingBuf stg{};
            stg.buf  = e.stg_buf;
            stg.size = e.stg_size;

            if (e.is_cube)
                recordCubeTextureUpload(cmd, s, stg, e.face_stride, VK_IMAGE_LAYOUT_UNDEFINED);
            else {
                TextureCopyPlan copy_plan{};
                copy_plan.count = std::clamp<uint32_t>(
                    e.mip_copy_count,
                    1u,
                    rdesc::kTextureMaxMipCount);
                for (uint32_t i = 0; i < copy_plan.count; ++i)
                {
                    copy_plan.regions[i].buffer_offset = e.mip_copies[i].buffer_offset;
                    copy_plan.regions[i].mip_level = e.mip_copies[i].mip_level;
                    copy_plan.regions[i].width = e.mip_copies[i].width;
                    copy_plan.regions[i].height = e.mip_copies[i].height;
                }
                recordTextureUploadInternal(cmd, s, stg, e.do_mips, &copy_plan, VK_IMAGE_LAYOUT_UNDEFINED);
            }
        }
        pending_staging_textures_.clear();
    }

    void BindlessCombinedSet::recordMipGenForSlot(VkCommandBuffer cmd, uint32_t slot_index)
    {
        auto& s = slots_[slot_index];
        if (s.mip_levels > 1)
            genMipsLinear(cmd, s);
        else
            barrierImage(cmd, s.image, image_aspect_,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void BindlessCombinedSet::pushDeferredMipGen(uint32_t slot_index)
    {
        pending_mip_gen_slots_.push_back(slot_index);
    }

    void BindlessCombinedSet::recordDeferredMipGens(VkCommandBuffer cmd)
    {
        if (pending_mip_gen_slots_.empty()) return;
        for (uint32_t idx : pending_mip_gen_slots_)
            recordMipGenForSlot(cmd, idx);
        pending_mip_gen_slots_.clear();
    }

} // namespace lux::render
