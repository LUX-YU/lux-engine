#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp> // vkFormatMipBytes (update validation)
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <vk_mem_alloc.h>

#include <limits>
#include <mutex>

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
            case VK_FORMAT_R8G8B8A8_UNORM:
                return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return 8;
            case VK_FORMAT_R8G8_UNORM:
                return 2;
            case VK_FORMAT_R8_UNORM:
                return 1;
            case VK_FORMAT_R16_UINT:
                return 2;
            case VK_FORMAT_R16_UNORM:
                return 2;
            default:
                return 0;
            }
        }
    }

    struct BindlessCombinedSet::Backing
    {
        DescriptorPoolOwner pool_owner;
        VkDescriptorPool pool{};
        VkDescriptorSet set{};
        std::uint32_t capacity{};
        std::uint32_t layout_capacity{};
        VmaImage image;
        ImageViewOwner view;
        SamplerOwner sampler;
    };

    BindlessCombinedSet::CreateResult BindlessCombinedSet::create(const BindlessSetCreateInfo& info) noexcept
    {
        const bool is_missing_binding = !info.resource_context || !info.deferred_queue || !info.descriptor_set_layout;
        const bool is_invalid_capacity = info.layout_max_capacity == 0 || info.initial_capacity == 0;
        const bool is_incomplete_external =
            (info.external_pool == VK_NULL_HANDLE) != (info.external_set == VK_NULL_HANDLE);
        const bool is_invalid_configuration =
            is_missing_binding || is_invalid_capacity || is_incomplete_external || info.frames_in_flight == 0;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidBindlessConfiguration>();
        }

        auto& resources = *info.resource_context;
        const VkDevice device = resources.logicalDevice();
        const auto& limits = resources.physicalDevice().descriptorIndexingProperties();
        Backing backing;
        backing.layout_capacity = std::min(
            {info.layout_max_capacity,
             limits.maxDescriptorSetUpdateAfterBindSampledImages,
             limits.maxPerStageDescriptorUpdateAfterBindSampledImages,
             limits.maxPerStageUpdateAfterBindResources}
        );
        if (backing.layout_capacity == 0)
        {
            return renderFailure<err::memory::CapacityExhausted>();
        }
        backing.capacity = std::min(info.initial_capacity, backing.layout_capacity);
        if (info.external_set)
        {
            backing.pool = info.external_pool;
            backing.set = info.external_set;
            backing.capacity = backing.layout_capacity;
        }
        else
        {
            constexpr std::uint32_t set_count = kMaxFramesInFlight + 2u;
            if (backing.layout_capacity > std::numeric_limits<std::uint32_t>::max() / set_count)
            {
                return renderFailure<err::memory::CapacityExhausted>();
            }
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, backing.layout_capacity * set_count};
            VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.flags =
                VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool_info.maxSets = set_count;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &size;
            auto pool = DescriptorPoolOwner::create(device, pool_info);
            if (!pool)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(pool.error()));
            }
            backing.pool_owner = std::move(*pool);
            backing.pool = backing.pool_owner.get();
            auto set = allocateSet(resources, backing.pool, info.descriptor_set_layout, backing.capacity);
            if (!set)
            {
                return lux::cxx::unexpected(set.error());
            }
            backing.set = *set;
        }

        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent = {1, 1, 1};
        image_info.mipLevels = image_info.arrayLayers = 1;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        auto image = VmaImage::create(resources.vmaAllocator(), image_info, allocation_info);
        if (!image)
        {
            return lux::cxx::unexpected(image.error());
        }
        backing.image = std::move(*image);
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = backing.image.image();
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto view = ImageViewOwner::create(device, view_info);
        if (!view)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(view.error()));
        }
        backing.view = std::move(*view);
        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        auto sampler = SamplerOwner::create(device, sampler_info);
        if (!sampler)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(sampler.error()));
        }
        backing.sampler = std::move(*sampler);

        auto command = beginOneTime(resources);
        if (!command)
        {
            return lux::cxx::unexpected(command.error());
        }
        barrierImage(
            command->get(),
            backing.image.image(),
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        auto completed = endOneTime(resources, std::move(*command));
        if (!completed)
        {
            return lux::cxx::unexpected(completed.error());
        }

        return std::unique_ptr<BindlessCombinedSet>(new BindlessCombinedSet(info, std::move(backing)));
    }

    BindlessCombinedSet::BindlessCombinedSet(const BindlessSetCreateInfo& info, Backing&& backing) noexcept
        : rc_(*info.resource_context), deferred_queue_(*info.deferred_queue),
          pool_owner_(std::move(backing.pool_owner)), set_layout_(info.descriptor_set_layout), desc_pool_(backing.pool),
          descriptor_set_(backing.set), set_index_(info.set_index), binding_(info.binding),
          layout_max_cap_(backing.layout_capacity), cur_cap_(backing.capacity), slots_(backing.layout_capacity),
          gen_(backing.layout_capacity, 1), alive_(backing.layout_capacity, 0), clear_on_remove_(info.clear_on_remove),
          default_format_(info.default_image_format), srgb_for_color_(info.srgb_for_color),
          gen_mips_(info.generate_mipmaps), image_tiling_(info.image_tiling), image_usage_(info.image_usage),
          view_type_(info.view_type), image_aspect_(info.aspect), default_sampler_ci_(info.default_sampler_ci),
          deferred_staging_(info.frames_in_flight), frames_in_flight_(info.frames_in_flight),
          fallback_image_(std::move(backing.image)), fallback_view_(std::move(backing.view)),
          fallback_sampler_(std::move(backing.sampler))
    {
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
    }

    BindlessCombinedSet::~BindlessCombinedSet() noexcept = default;

    Expected<SlotHandle> BindlessCombinedSet::addTexture(
        const rdesc::Texture& tex,
        const VkSamplerCreateInfo* opt_sampler_ci,
        VkFormat fmt,
        std::optional<bool> gen_mips,
        std::optional<bool> srgb_override
    )
    {
        auto room = ensureRoom();
        if (!room)
        {
            return lux::cxx::unexpected(room.error());
        }
        const int w = tex.width(), h = tex.height(), c = tex.channel();
        const bool is_invalid_extent = w <= 0 || h <= 0;
        const bool is_invalid_channels = c < 1 || c > 4;
        const bool is_invalid_texture = is_invalid_extent || is_invalid_channels || tex.size() == 0;
        if (is_invalid_texture)
        {
            return renderFailure<err::asset::Invalid>();
        }

        SampledImage s{};
        s.width = w;
        s.height = h;
        s.format = pickFormat(c, fmt, srgb_override.value_or(srgb_for_color_));

        TextureCopyPlan copy_plan{};
        const uint32_t provided_mips = std::clamp<uint32_t>(tex.mipCount(), 1u, rdesc::kTextureMaxMipCount);
        const bool has_provided_mips = provided_mips > 1;
        const bool do_mips = gen_mips.value_or(gen_mips_) && !isBlockCompressedVkFormat(s.format) && !has_provided_mips;
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
        const VkSamplerCreateInfo& sampler = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        auto prepared = createSampledImage(s, sampler);
        if (!prepared)
        {
            return lux::cxx::unexpected(prepared.error());
        }

        auto staging = createStaging(tex.size(), tex.data());
        if (!staging)
        {
            return lux::cxx::unexpected(staging.error());
        }

        // Assign slot and write descriptor (UPDATE_AFTER_BIND, valid before upload)
        uint32_t idx = allocIndex();
        writeCombinedDescriptor(idx, s.view.get(), s.sampler.get());
        slots_[idx] = std::move(s);
        alive_[idx] = 1;

        // Defer GPU upload to batch (render-thread only; no lock needed)
        {
            pending_uploads_.push_back(PendingUpload{std::move(*staging), idx, do_mips, false, 0, copy_plan});
        }
        return SlotHandle{idx, gen_[idx]};
    }

    Expected<SlotHandle> BindlessCombinedSet::addPersistentTexture(
        uint32_t width,
        uint32_t height,
        uint32_t mip_levels,
        VkFormat fmt,
        const VkSamplerCreateInfo* opt_sampler_ci
    )
    {
        assert(view_type_ == VK_IMAGE_VIEW_TYPE_2D && "addPersistentTexture() targets the 2D bindless set");
        const bool is_missing_width = width == 0;
        const bool is_missing_height = height == 0;
        const bool is_missing_mips = mip_levels == 0;
        const bool is_undefined_format = fmt == VK_FORMAT_UNDEFINED;
        const bool is_invalid_descriptor =
            is_missing_width || is_missing_height || is_missing_mips || is_undefined_format;
        if (is_invalid_descriptor)
            return renderFailure<err::internal::InvalidArgument>();
        auto room = ensureRoom();
        if (!room)
        {
            return lux::cxx::unexpected(room.error());
        }

        SampledImage s{};
        s.width = static_cast<int>(width);
        s.height = static_cast<int>(height);
        s.format = fmt;
        s.mip_levels = std::min<uint32_t>(mip_levels, calcMipLevels(width, height));
        s.array_layers = 1;

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
            const uint32_t mw = std::max(1u, width >> m);
            const uint32_t mh = std::max(1u, height >> m);
            plan.regions[m].buffer_offset = total;
            plan.regions[m].mip_level = m;
            plan.regions[m].width = mw;
            plan.regions[m].height = mh;
            total += static_cast<VkDeviceSize>(mw) * mh * texel;
        }
        const std::vector<std::byte> zeros(static_cast<size_t>(total)); // value-init = 0
        const VkSamplerCreateInfo& sampler = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        auto prepared = createSampledImage(s, sampler);
        if (!prepared)
        {
            return lux::cxx::unexpected(prepared.error());
        }

        auto staging = createStaging(total, zeros.data());
        if (!staging)
        {
            return lux::cxx::unexpected(staging.error());
        }
        const uint32_t idx = allocIndex();
        writeCombinedDescriptor(idx, s.view.get(), s.sampler.get());
        slots_[idx] = std::move(s);
        alive_[idx] = 1;

        pending_uploads_.push_back(PendingUpload{
            std::move(*staging),
            idx,
            /*do_mips=*/false,
            /*is_cube=*/false,
            0,
            plan,
            VK_IMAGE_LAYOUT_UNDEFINED,
        });
        return SlotHandle{idx, gen_[idx]};
    }

    bool BindlessCombinedSet::updateTextureRegions(
        const SlotHandle& h,
        std::span<const RegionUpdate> regions,
        std::span<const std::byte> pixels,
        uint32_t texel_bytes
    )
    {
        if (!isTextureAlive(h) || regions.empty() || texel_bytes == 0)
            return false;
        const uint32_t idx = h.index;

        // Ride the NORMAL pending-upload pipeline in ≤kTextureMaxMipCount-region
        // chunks (TextureCopyPlan's capacity): rows are repacked TIGHTLY into each
        // chunk's staging buffer, so VkBufferImageCopy never needs bufferRowLength
        // (and the wire row_pitch has no texel-alignment constraint).
        std::vector<PendingUpload> prepared;
        std::size_t next = 0;
        while (next < regions.size())
        {
            const uint32_t count =
                static_cast<uint32_t>(std::min<std::size_t>(regions.size() - next, rdesc::kTextureMaxMipCount));

            TextureCopyPlan plan{};
            plan.count = count;
            VkDeviceSize total = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const RegionUpdate& r = regions[next + i];
                plan.regions[i].buffer_offset = total;
                plan.regions[i].mip_level = r.mip;
                plan.regions[i].width = r.width;
                plan.regions[i].height = r.height;
                plan.regions[i].x = r.x;
                plan.regions[i].y = r.y;
                plan.regions[i].array_layer = r.array_layer;
                total += static_cast<VkDeviceSize>(r.width) * r.height * texel_bytes;
            }

            std::vector<std::byte> packed(static_cast<size_t>(total));
            VkDeviceSize out = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const RegionUpdate& r = regions[next + i];
                const std::size_t tight = static_cast<std::size_t>(r.width) * texel_bytes;
                const std::size_t pitch = r.row_pitch_bytes ? r.row_pitch_bytes : tight;
                const std::byte* src = pixels.data() + r.data_offset;
                for (uint32_t row = 0; row < r.height; ++row)
                {
                    std::memcpy(
                        packed.data() + static_cast<size_t>(out),
                        src + static_cast<std::size_t>(row) * pitch,
                        tight
                    );
                    out += static_cast<VkDeviceSize>(tight);
                }
            }

            auto staging = createStaging(total, packed.data());

            if (!staging)
            {
                return false;
            }
            prepared.push_back(PendingUpload{
                std::move(*staging),
                idx,
                /*do_mips=*/false,
                /*is_cube=*/false,
                0,
                plan,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, // persistent textures are always initialized
            });
            next += count;
        }
        pending_uploads_.insert(
            pending_uploads_.end(),
            std::make_move_iterator(prepared.begin()),
            std::make_move_iterator(prepared.end())
        );
        return true;
    }

    Expected<SlotHandle> BindlessCombinedSet::addCubeTexture(
        const rdesc::Texture faces[6],
        const VkSamplerCreateInfo* opt_sampler_ci,
        VkFormat fmt
    )
    {
        assert(view_type_ == VK_IMAGE_VIEW_TYPE_CUBE && "addCubeTexture() requires a cube-view BindlessCombinedSet");
        auto room = ensureRoom();
        if (!room)
        {
            return lux::cxx::unexpected(room.error());
        }

        const int w = faces[0].width(), h = faces[0].height(), c = faces[0].channel();
        assert(w > 0 && h > 0 && w == h && (c == 1 || c == 2 || c == 3 || c == 4));

        SampledImage s{};
        s.width = w;
        s.height = h;
        s.format = pickFormat(c, fmt);
        s.mip_levels = 1; // cubemap mipmaps not yet supported
        s.array_layers = 6;

        // Combine all 6 faces into a single contiguous staging buffer
        const VkDeviceSize face_size = faces[0].size();
        const VkDeviceSize total_size = face_size * 6;
        std::vector<uint8_t> combined(total_size);
        for (int i = 0; i < 6; ++i)
        {
            assert(faces[i].width() == w && faces[i].height() == h && faces[i].channel() == c);
            std::memcpy(combined.data() + i * face_size, faces[i].data(), face_size);
        }
        const VkSamplerCreateInfo& sampler = opt_sampler_ci ? *opt_sampler_ci : default_sampler_ci_;
        auto prepared = createSampledImage(s, sampler);
        if (!prepared)
        {
            return lux::cxx::unexpected(prepared.error());
        }

        auto staging = createStaging(total_size, combined.data());
        if (!staging)
        {
            return lux::cxx::unexpected(staging.error());
        }

        uint32_t idx = allocIndex();
        writeCombinedDescriptor(idx, s.view.get(), s.sampler.get());
        slots_[idx] = std::move(s);
        alive_[idx] = 1;

        {
            pending_uploads_.push_back(PendingUpload{std::move(*staging), idx, false, true, face_size, {}});
        }
        return SlotHandle{idx, gen_[idx]};
    }

    Expected<void> BindlessCombinedSet::flushUploads()
    {
        if (pending_uploads_.empty())
            return {};
        auto command = beginOneTime(rc_);
        if (!command)
        {
            return lux::cxx::unexpected(command.error());
        }
        recordPendingUploads(command->get());
        auto completed = endOneTime(rc_, std::move(*command));
        // endOneTime establishes completion/device-loss safety even if wait itself fails.
        one_shot_staging_.clear();
        return completed;
    }

    void BindlessCombinedSet::flushUploads(VkCommandBuffer cb, uint32_t fi)
    {
        if (pending_uploads_.empty())
            return;
        recordPendingUploads(cb);
        // Per-frame path: move collected staging into the frame-slot array.
        auto& slot = deferred_staging_[fi % frames_in_flight_];
        slot.insert(
            slot.end(),
            std::make_move_iterator(one_shot_staging_.begin()),
            std::make_move_iterator(one_shot_staging_.end())
        );
        one_shot_staging_.clear();
    }

    void BindlessCombinedSet::recordPendingUploads(VkCommandBuffer cb)
    {
        std::vector<PendingUpload> to_flush = std::move(pending_uploads_);

        for (auto& p : to_flush)
        {
            auto& s = slots_[p.slot_index];
            if (p.is_cube)
                recordCubeTextureUpload(cb, s, p.staging.buffer(), p.face_stride, p.old_layout);
            else
                recordTextureUploadInternal(cb, s, p.staging.buffer(), p.do_mips, &p.copy_plan, p.old_layout);
        }

        // Collect staging into temporary; caller decides where they go.
        for (auto& p : to_flush)
            one_shot_staging_.push_back(std::move(p.staging));
    }

    void BindlessCombinedSet::retireDeferredStaging(uint32_t fi)
    {
        auto& slot = deferred_staging_[fi % frames_in_flight_];
        slot.clear();
    }

    // =========================================================================
    //  Transfer scheduler integration
    // =========================================================================

    void BindlessCombinedSet::submitTransfers(TransferScheduler& scheduler, uint32_t fi)
    {
        // ── Process pending_uploads_ (from synchronous texture creation and updates) ─
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
                        req.src = p.staging.buffer();
                        req.src_offset = static_cast<VkDeviceSize>(face) * p.face_stride;
                        req.dst = s.image.image();
                        req.old_layout = p.old_layout;
                        req.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = {image_aspect_, 0, face, 1};
                        req.offset = {0, 0, 0};
                        req.extent = {static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height), 1};
                        req.domain = EBufferDomain::SAMPLED_FS;
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
                        fallback.regions[0].mip_level = 0;
                        fallback.regions[0].width = static_cast<uint32_t>(std::max(1, s.width));
                        fallback.regions[0].height = static_cast<uint32_t>(std::max(1, s.height));
                        plan = &fallback;
                    }

                    // No mip clamp on count — see recordTextureUploadInternal:
                    // region updates carry many mip-0 rects; clamping dropped
                    // all but the first on 1-mip textures (stale-trail bug).
                    const uint32_t copy_count = plan->count;
                    const bool runtime_mips = p.do_mips && s.mip_levels > 1 && copy_count == 1;

                    for (uint32_t i = 0; i < copy_count; ++i)
                    {
                        const auto& cr = plan->regions[i];
                        ImageCopyRequest req{};
                        req.src = p.staging.buffer();
                        req.src_offset = cr.buffer_offset;
                        req.dst = s.image.image();
                        req.old_layout = p.old_layout;
                        req.new_layout = runtime_mips ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = {image_aspect_, std::min(cr.mip_level, s.mip_levels - 1), cr.array_layer, 1};
                        req.offset = {static_cast<int32_t>(cr.x), static_cast<int32_t>(cr.y), 0};
                        req.extent = {std::max(1u, cr.width), std::max(1u, cr.height), 1};
                        req.domain = EBufferDomain::SAMPLED_FS;
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
                        req.src = e.stg_buf;
                        req.src_offset = static_cast<VkDeviceSize>(face) * e.face_stride;
                        req.dst = s.image.image();
                        req.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                        req.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = {image_aspect_, 0, face, 1};
                        req.offset = {0, 0, 0};
                        req.extent = {static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height), 1};
                        req.domain = EBufferDomain::SAMPLED_FS;
                        scheduler.submitImageCopy(req);
                    }
                }
                else
                {
                    TextureCopyPlan copy_plan{};
                    copy_plan.count = std::clamp<uint32_t>(e.mip_copy_count, 1u, rdesc::kTextureMaxMipCount);
                    for (uint32_t i = 0; i < copy_plan.count; ++i)
                    {
                        copy_plan.regions[i].buffer_offset = e.mip_copies[i].buffer_offset;
                        copy_plan.regions[i].mip_level = e.mip_copies[i].mip_level;
                        copy_plan.regions[i].width = e.mip_copies[i].width;
                        copy_plan.regions[i].height = e.mip_copies[i].height;
                    }

                    const uint32_t copy_count = std::min(copy_plan.count, s.mip_levels);
                    const bool runtime_mips = e.do_mips && s.mip_levels > 1 && copy_count == 1;

                    for (uint32_t i = 0; i < copy_count; ++i)
                    {
                        const auto& cr = copy_plan.regions[i];
                        ImageCopyRequest req{};
                        req.src = e.stg_buf;
                        req.src_offset = cr.buffer_offset;
                        req.dst = s.image.image();
                        req.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                        req.new_layout = runtime_mips ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        req.subresource = {image_aspect_, std::min(cr.mip_level, s.mip_levels - 1), cr.array_layer, 1};
                        req.offset = {static_cast<int32_t>(cr.x), static_cast<int32_t>(cr.y), 0};
                        req.extent = {std::max(1u, cr.width), std::max(1u, cr.height), 1};
                        req.domain = EBufferDomain::SAMPLED_FS;
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
                req.kind = QFOTAcquireRequest::EKind::IMAGE;
                req.image = b.image;
                req.img_layout = b.oldLayout;
                req.img_range = b.subresourceRange;
                req.src_family = b.srcQueueFamilyIndex;
                req.dst_family = b.dstQueueFamilyIndex;
                req.domain = EBufferDomain::SAMPLED_FS;
                scheduler.submitQFOTAcquire(req);
            }
            pending_acquire_barriers_.clear();
        }
    }

    void BindlessCombinedSet::postTransfer(VkCommandBuffer cmd)
    {
        recordDeferredMipGens(cmd);
    }

    bool BindlessCombinedSet::removeTexture(const SlotHandle& h)
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

        pending_recycle_.emplace_back(
            idx,
            deferred_queue_.currentSerial()
        ); // standalone/tests (waitIdle): immediate reuse is safe

        return true;
    }

    // =========================================================================
    // A1: Render-thread slot finalize + recycle API
    // =========================================================================

    bool BindlessCombinedSet::updateTextureMips(
        const SlotHandle& h,
        std::span<const TextureUpdateMip> mips,
        bool generate_mips
    )
    {
        if (!isTextureAlive(h) || mips.empty())
            return false;

        const uint32_t idx = h.index;
        SampledImage& s = slots_[idx];
        if (s.array_layers != 1)
            return false;

        const uint32_t copy_count =
            std::clamp<uint32_t>(static_cast<uint32_t>(mips.size()), 1u, rdesc::kTextureMaxMipCount);
        if (copy_count > s.mip_levels)
            return false;

        const auto& base = mips[0];
        if (base.width == 0 || base.height == 0)
            return false;
        if (static_cast<int32_t>(base.width) != s.width || static_cast<int32_t>(base.height) != s.height)
            return false;

        const bool do_runtime_mips =
            generate_mips && copy_count == 1 && s.mip_levels > 1 && !isBlockCompressedVkFormat(s.format);

        TextureCopyPlan copy_plan{};
        copy_plan.count = copy_count;

        const uint32_t sw = static_cast<uint32_t>(s.width);
        const uint32_t sh = static_cast<uint32_t>(s.height);
        VkDeviceSize total_bytes = 0;
        for (uint32_t i = 0; i < copy_count; ++i)
        {
            const auto& mip = mips[i];
            const bool is_missing_data = mip.data == nullptr;
            const bool is_empty_data = mip.bytes == 0;
            const bool is_missing_width = mip.width == 0;
            const bool is_missing_height = mip.height == 0;
            const bool is_invalid_data = is_missing_data || is_empty_data || is_missing_width || is_missing_height;
            if (is_invalid_data)
                return false;
            // each mip must match the EXISTING slot's mip-chain extent
            // (max(1, base>>i)) and hold EXACTLY the format-computed byte count. Otherwise
            // the buffer→image copy reads past the provided data (OOB) or writes wrong
            // bytes — the create path is strict, so updates must be too.
            const uint32_t ew = (sw >> i) ? (sw >> i) : 1u;
            const uint32_t eh = (sh >> i) ? (sh >> i) : 1u;
            const std::uint64_t need = vkFormatMipBytes(s.format, ew, eh);
            const bool is_invalid_width = mip.width != ew;
            const bool is_invalid_height = mip.height != eh;
            const bool is_invalid_format_size = need == 0;
            const bool is_invalid_byte_count = static_cast<std::uint64_t>(mip.bytes) != need;
            const bool is_invalid_mip =
                is_invalid_width || is_invalid_height || is_invalid_format_size || is_invalid_byte_count;
            if (is_invalid_mip)
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
            std::memcpy(packed.data() + static_cast<size_t>(offset), mip.data, mip.bytes);
            offset += static_cast<VkDeviceSize>(mip.bytes);
        }

        auto staging = createStaging(total_bytes, packed.data());

        if (!staging)
        {
            return false;
        }
        pending_uploads_.push_back(PendingUpload{
            std::move(*staging),
            idx,
            do_runtime_mips,
            false,
            0,
            copy_plan,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        return true;
    }

    bool BindlessCombinedSet::updateCubeFaces(const SlotHandle& h, const std::array<TextureUpdateFace, 6>& faces)
    {
        if (!isTextureAlive(h))
            return false;

        const uint32_t idx = h.index;
        SampledImage& s = slots_[idx];
        if (s.array_layers != 6)
            return false;

        // each face must be EXACTLY the format-computed size for the slot's
        // format and face extent — the old check only ensured the six faces AGREE, which
        // let a wrong-but-consistent size through and could OOB the per-face copy.
        const VkDeviceSize expected_face = static_cast<VkDeviceSize>(
            vkFormatMipBytes(s.format, static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height))
        );
        if (expected_face == 0)
            return false;
        for (const auto& face : faces)
        {
            if (face.data == nullptr || static_cast<VkDeviceSize>(face.bytes) != expected_face)
                return false;
        }
        const VkDeviceSize face_stride = expected_face;

        const VkDeviceSize total_bytes = face_stride * 6;
        std::vector<std::byte> packed;
        packed.resize(static_cast<size_t>(total_bytes));

        VkDeviceSize offset = 0;
        for (const auto& face : faces)
        {
            std::memcpy(packed.data() + static_cast<size_t>(offset), face.data, face.bytes);
            offset += static_cast<VkDeviceSize>(face.bytes);
        }

        auto staging = createStaging(total_bytes, packed.data());

        if (!staging)
        {
            return false;
        }
        pending_uploads_.push_back(PendingUpload{
            std::move(*staging),
            idx,
            false,
            true,
            face_stride,
            {},
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        return true;
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
        return {idx, gen_[idx]};
    }

    // ---------- finalizeTransferredTexture ----------
    void BindlessCombinedSet::finalizeTransferredTexture(uint32_t slot_idx, SampledImage image)
    {
        slots_[slot_idx] = std::move(image);
        const auto& slot = slots_[slot_idx];
        writeCombinedDescriptor(slot_idx, slot.view.get(), slot.sampler.get());
    }

    void BindlessCombinedSet::replaceTransferredTexture(uint32_t slot_idx, SampledImage image)
    {
        assert(slot_idx < cur_cap_ && alive_[slot_idx]);
        SampledImage previous = std::move(slots_[slot_idx]);
        slots_[slot_idx] = std::move(image);
        const auto& slot = slots_[slot_idx];
        // Future descriptor reads use the new allocation; submitted frames retain the old one.
        writeCombinedDescriptor(slot_idx, slot.view.get(), slot.sampler.get());
        retireCombinedDeferred(previous);
    }

    Expected<void> BindlessCombinedSet::reserve(uint32_t need)
    {
        if (need <= cur_cap_)
            return {};
        const bool is_fixed_or_full = !pool_owner_ || need > layout_max_cap_;
        if (is_fixed_or_full)
        {
            return renderFailure<err::memory::CapacityExhausted>();
        }
        uint32_t new_cap = std::min(nextCap(cur_cap_, need), layout_max_cap_);

        assert(new_cap <= layout_max_cap_ && "Need to rebuild layout with larger descriptorCount");
        auto reallocated = reallocateSetAndCopy(new_cap);
        if (!reallocated)
        {
            return reallocated;
        }
        if (slots_.size() < new_cap)
            slots_.resize(new_cap);
        if (gen_.size() < new_cap)
            gen_.resize(new_cap, 1);
        if (alive_.size() < new_cap)
            alive_.resize(new_cap, 0);
        cur_cap_ = new_cap;
        return {};
    }

    // ===== Private methods =====

    Expected<VkDescriptorSet> BindlessCombinedSet::allocateSet(
        ResourceContext& resources,
        VkDescriptorPool pool,
        VkDescriptorSetLayout layout,
        std::uint32_t count
    ) noexcept
    {
        VkDescriptorSetVariableDescriptorCountAllocateInfo counts{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
        };
        counts.descriptorSetCount = 1;
        counts.pDescriptorCounts = &count;
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        info.pNext = &counts;
        VkDescriptorSet set{};
        VK_EXPECT(vkAllocateDescriptorSets(resources.logicalDevice(), &info, &set));
        return set;
    }

    Expected<void> BindlessCombinedSet::reallocateSetAndCopy(uint32_t newCount)
    {
        auto& device = rc_.logicalDevice();

        auto allocated = allocateSet(rc_, desc_pool_, set_layout_, newCount);
        if (!allocated)
        {
            return lux::cxx::unexpected(allocated.error());
        }
        const auto newSet = *allocated;

        std::vector<VkCopyDescriptorSet> copies;
        copies.reserve(count_);
        for (uint32_t i = 0; i < count_; ++i)
        {
            const bool is_live_slot = i < alive_.size() && alive_[i];
            if (is_live_slot)
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

        deferred_queue_.retireDescriptorSet(desc_pool_, descriptor_set_);
        descriptor_set_ = newSet;
        return {};
    }

    Expected<void> BindlessCombinedSet::ensureRoom()
    {
        const bool has_room = !free_.empty() || count_ < cur_cap_;
        if (has_room)
        {
            return {};
        }
        return reserve(count_ + 1);
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
        vkUpdateDescriptorSets(rc_.logicalDevice(), 1, &w, 0, nullptr);
    }

    void BindlessCombinedSet::writeCombinedDescriptorNull(uint32_t idx)
    {
        VkDescriptorImageInfo di{};
        di.sampler = fallback_sampler_.get();
        di.imageView = fallback_view_.get();
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descriptor_set_;
        w.dstBinding = binding_;
        w.dstArrayElement = idx;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &di;
        vkUpdateDescriptorSets(rc_.logicalDevice(), 1, &w, 0, nullptr);
    }

    Expected<void> BindlessCombinedSet::createImageGPU(SampledImage& s)
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

        auto image = VmaImage::create(rc_.vmaAllocator(), ici, aci);
        if (!image)
        {
            return lux::cxx::unexpected(image.error());
        }
        s.image = std::move(*image);
        return {};
    }

    Expected<void> BindlessCombinedSet::createImageView(SampledImage& s)
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s.image.image();
        vci.viewType = view_type_;
        vci.format = s.format;
        vci.subresourceRange = {image_aspect_, 0, s.mip_levels, 0, s.array_layers};
        auto view = ImageViewOwner::create(rc_.logicalDevice(), vci);
        if (!view)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(view.error()));
        }
        s.view = std::move(*view);
        return {};
    }

    Expected<void> BindlessCombinedSet::createSampledImage(SampledImage& slot, const VkSamplerCreateInfo& info)
    {
        auto image = createImageGPU(slot);
        if (!image)
        {
            return image;
        }
        auto view = createImageView(slot);
        if (!view)
        {
            return view;
        }
        auto sampler = SamplerOwner::create(rc_.logicalDevice(), info);
        if (!sampler)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(sampler.error()));
        }
        slot.sampler = std::move(*sampler);
        return {};
    }

    void BindlessCombinedSet::retireCombinedDeferred(SampledImage& slot)
    {
        if (slot.sampler)
        {
            deferred_queue_.retireSampler(slot.sampler.release());
        }
        if (slot.view)
        {
            deferred_queue_.retireImageView(slot.view.release());
        }
        if (slot.image)
        {
            const auto allocation = slot.image.release();
            deferred_queue_.retireImage(allocation.image, allocation.allocation);
        }
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

    Expected<StagingBuffer> BindlessCombinedSet::createStaging(VkDeviceSize size, const void* data)
    {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VmaAllocationInfo mapped{};
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VK_EXPECT(vmaCreateBuffer(rc_.vmaAllocator(), &buffer_info, &allocation_info, &buffer, &allocation, &mapped));
        StagingBuffer owner(rc_.vmaAllocator(), buffer, allocation);
        if (!mapped.pMappedData)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
        }
        std::memcpy(mapped.pMappedData, data, static_cast<size_t>(size));
        VK_EXPECT(vmaFlushAllocation(rc_.vmaAllocator(), allocation, 0, size));
        return owner;
    }

    Expected<CommandBufferOwner> BindlessCombinedSet::beginOneTime(ResourceContext& resources)
    {
        auto command = CommandBufferOwner::create(resources.logicalDevice(), resources.commandPool());
        if (!command)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(command.error()));
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_EXPECT(vkBeginCommandBuffer(command->get(), &bi));
        return std::move(*command);
    }

    Expected<void> BindlessCombinedSet::endOneTime(ResourceContext& resources, CommandBufferOwner command)
    {
        const auto cb = command.get();
        VK_EXPECT(vkEndCommandBuffer(cb));
        VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto fence_owner = FenceOwner::create(resources.logicalDevice(), fence_ci);
        if (!fence_owner)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(fence_owner.error()));
        }
        const auto fence = fence_owner->get();

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VkResult submit_res{VK_ERROR_UNKNOWN};
        {
            const std::scoped_lock queue_lock(resources.deviceContext().graphicsQueueMutex());
            submit_res = vkQueueSubmit(resources.graphicsQueue(), 1, &si, fence);
        }
        if (submit_res != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(submit_res));
        }
        const auto wait_res = vkWaitForFences(resources.logicalDevice(), 1, &fence, VK_TRUE, UINT64_MAX);
        if (wait_res != VK_SUCCESS)
        {
            // A failed fence wait alone does not authorize destruction of submitted work.
            // This is already an explicitly synchronous path. Preserve ownership until
            // device idle or confirmed device loss, as in the renderer shutdown boundary.
            const auto idle = resources.deviceContext().waitIdle();
            const bool can_release = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST;
            if (!can_release)
            {
                renderFatal("One-time upload could not establish a safe resource release point");
            }
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(wait_res));
        }
        return {};
    }

    void BindlessCombinedSet::barrierImage(
        VkCommandBuffer cb,
        VkImage img,
        VkImageAspectFlags aspect,
        VkImageLayout oldL,
        VkImageLayout newL,
        uint32_t base,
        uint32_t levels,
        uint32_t layer_count
    )
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

    void BindlessCombinedSet::genMipsLinear(VkCommandBuffer cb, SampledImage& s, VkImageLayout untouched_old_layout)
    {
        int32_t w = s.width, h = s.height;
        for (uint32_t i = 1; i < s.mip_levels; ++i)
        {
            barrierImage(
                cb,
                s.image.image(),
                image_aspect_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                i - 1,
                1
            );
            int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);

            VkImageBlit bl{};
            bl.srcSubresource = {image_aspect_, i - 1, 0, 1};
            bl.srcOffsets[0] = {0, 0, 0};
            bl.srcOffsets[1] = {w, h, 1};
            bl.dstSubresource = {image_aspect_, i, 0, 1};
            bl.dstOffsets[0] = {0, 0, 0};
            bl.dstOffsets[1] = {nw, nh, 1};

            barrierImage(
                cb,
                s.image.image(),
                image_aspect_,
                untouched_old_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                i,
                1
            );
            vkCmdBlitImage(
                cb,
                s.image.image(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                s.image.image(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &bl,
                VK_FILTER_LINEAR
            );

            w = nw;
            h = nh;
        }

        if (s.mip_levels > 1)
        {
            barrierImage(
                cb,
                s.image.image(),
                image_aspect_,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0,
                s.mip_levels - 1
            );
        }
        barrierImage(
            cb,
            s.image.image(),
            image_aspect_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            s.mip_levels - 1,
            1
        );
    }

    void BindlessCombinedSet::recordTextureUploadInternal(
        VkCommandBuffer cb,
        SampledImage& s,
        VkBuffer staging,
        bool do_mips,
        const BindlessCombinedSet::TextureCopyPlan* copy_plan,
        VkImageLayout old_layout
    )
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

        barrierImage(
            cb,
            s.image.image(),
            image_aspect_,
            old_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0,
            transition_levels
        );

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
                cr.array_layer, // 0 for full-mip uploads; region updates target a layer
                1
            };
            r.imageOffset = {
                static_cast<int32_t>(cr.x), // 0 for full-mip uploads
                static_cast<int32_t>(cr.y),
                0
            };
            r.imageExtent = {std::max(1u, cr.width), std::max(1u, cr.height), 1};
            copies.push_back(r);
        }
        vkCmdCopyBufferToImage(
            cb,
            staging,
            s.image.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(copies.size()),
            copies.data()
        );

        if (runtime_mips)
            genMipsLinear(cb, s, old_layout);
        else
            barrierImage(
                cb,
                s.image.image(),
                image_aspect_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                0,
                s.mip_levels
            );
    }

    void BindlessCombinedSet::recordCubeTextureUpload(
        VkCommandBuffer cb,
        SampledImage& s,
        VkBuffer staging,
        VkDeviceSize face_stride,
        VkImageLayout old_layout
    )
    {
        // Transition all 6 layers to TRANSFER_DST
        barrierImage(
            cb,
            s.image.image(),
            image_aspect_,
            old_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0,
            s.mip_levels,
            s.array_layers
        );

        // Copy 6 faces from staging buffer (contiguous, one per layer)
        std::array<VkBufferImageCopy, 6> regions{};
        for (uint32_t face = 0; face < 6; ++face)
        {
            regions[face].bufferOffset = face * face_stride;
            regions[face].imageSubresource = {image_aspect_, 0, face, 1};
            regions[face].imageExtent = {(uint32_t)s.width, (uint32_t)s.height, 1};
        }
        vkCmdCopyBufferToImage(cb, staging, s.image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions.data());

        // Transition to SHADER_READ_ONLY
        barrierImage(
            cb,
            s.image.image(),
            image_aspect_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            0,
            s.mip_levels,
            s.array_layers
        );
    }

    // ---------- Async image acquire barriers ----------

    void BindlessCombinedSet::pushImageAcquireBarrier(
        VkImage image,
        uint32_t mip_levels,
        uint32_t array_layers,
        VkImageLayout layout,
        uint32_t src_queue_family,
        uint32_t dst_queue_family
    )
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = layout;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = src_queue_family;
        b.dstQueueFamilyIndex = dst_queue_family;
        b.image = image;
        b.subresourceRange = {image_aspect_, 0, mip_levels, 0, array_layers};
        pending_acquire_barriers_.push_back(b);
    }

    void BindlessCombinedSet::recordAcquireBarriers(VkCommandBuffer cmd)
    {
        if (pending_acquire_barriers_.empty())
            return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(pending_acquire_barriers_.size());
        dep.pImageMemoryBarriers = pending_acquire_barriers_.data();
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
        if (pending_staging_textures_.empty())
            return;

        for (auto& e : pending_staging_textures_)
        {
            auto& s = slots_[e.slot_index];
            const VkBuffer stg = e.stg_buf;

            if (e.is_cube)
                recordCubeTextureUpload(cmd, s, stg, e.face_stride, VK_IMAGE_LAYOUT_UNDEFINED);
            else
            {
                TextureCopyPlan copy_plan{};
                copy_plan.count = std::clamp<uint32_t>(e.mip_copy_count, 1u, rdesc::kTextureMaxMipCount);
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
            barrierImage(
                cmd,
                s.image.image(),
                image_aspect_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            );
    }

    void BindlessCombinedSet::pushDeferredMipGen(uint32_t slot_index)
    {
        pending_mip_gen_slots_.push_back(slot_index);
    }

    void BindlessCombinedSet::recordDeferredMipGens(VkCommandBuffer cmd)
    {
        if (pending_mip_gen_slots_.empty())
            return;
        for (uint32_t idx : pending_mip_gen_slots_)
            recordMipGenForSlot(cmd, idx);
        pending_mip_gen_slots_.clear();
    }

} // namespace lux::render
