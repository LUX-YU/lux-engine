#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::render
{
    // ------------------------------------------------------------------
    // Shared descriptor pool + set for both sampler2D[] (binding 0) and
    // samplerCube[] (binding 1) in the same descriptor set (set 2).
    // ------------------------------------------------------------------
    void TextureResources::createSharedPoolAndSet(
        VkDescriptorSetLayout layout,
        uint32_t tex2d_max,
        uint32_t cube_max)
    {
        auto& device = dc_->logicalDevice();
        // Exactly ONE set is ever allocated here — both BCS instances run in
        // external-set mode where the set never reallocates (retired-set arrays
        // were replaced by the DeferredDestroyQueue; the old kRetiredSetsMax
        // constant no longer exists). The previous 5x reservation
        // ((tex2d_max+cube_max)*5, maxSets=5) was dead budget. (P-1)
        constexpr uint32_t kRetiredMax = 0;

        // Pool sized for the single active set's two bindings.
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = (tex2d_max + cube_max) * (kRetiredMax + 1);

        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                  | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets      = kRetiredMax + 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &ps;
        VK_CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &shared_pool_));

        // Allocate one set — variable descriptor count applies to binding 1 (cube)
        uint32_t var_count = cube_max;
        VkDescriptorSetVariableDescriptorCountAllocateInfo vci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vci.descriptorSetCount = 1;
        vci.pDescriptorCounts  = &var_count;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = shared_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &layout;
        ai.pNext              = &vci;
        // These counts MUST match the Texture set layout's binding counts —
        // the driver charges the pool against the layout, not against what we
        // intended. They come from GeneralDescriptorSetLayout::bindless2DCount
        // / bindlessCubeCount for exactly that reason; deriving them a second
        // time here would reintroduce the mismatch that made this call return
        // VK_ERROR_OUT_OF_POOL_MEMORY on Adreno.
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, &shared_set_));
    }

    bool TextureResources::init(
            const InitInfo &info
    )
    {
        assert(info.device_context && info.graphics_queue && info.upload_cmd_pool);

        // Set BEFORE any allocation: shutdown() early-outs on this flag, so a
        // failure partway through would otherwise leave the shared pool/set and
        // whichever bindless set already succeeded unreclaimable. Flag first,
        // and let every failure path reclaim through shutdown().
        initialized_ = true;

        dc_ = info.device_context;
        queue_ = info.graphics_queue;
        upload_pool_ = info.upload_cmd_pool;
        slices_ = info.slices;

        combined_ci_ = info.combined_ci;

        default_sampler_ci_ = info.default_sampler_ci;
        if (default_sampler_ci_.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)
        {
            default_sampler_ci_ = {};
            default_sampler_ci_.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            default_sampler_ci_.magFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.minFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            default_sampler_ci_.addressModeU = default_sampler_ci_.addressModeV = default_sampler_ci_.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            default_sampler_ci_.maxLod = VK_LOD_CLAMP_NONE;
        }

        // -- Create shared pool + set for dual-binding texture set --
        const uint32_t tex2d_max = combined_ci_.layout_max_capacity;
        const uint32_t cube_max  = info.cube_max_capacity;
        createSharedPoolAndSet(combined_ci_.descriptor_set_layout, tex2d_max, cube_max);

        // -- Init 2D texture bindless set (binding 0, external set) --
        BCInitInfo ci_2d          = combined_ci_;
        ci_2d.external_pool       = shared_pool_;
        ci_2d.external_set        = shared_set_;
        ci_2d.initial_capacity    = tex2d_max;   // fixed (no reallocation in external mode)
        ci_2d.layout_max_capacity = tex2d_max;
        ci_2d.frames_in_flight    = slices_;
        if (!combined_.init(ci_2d))
        { shutdown(); return false; }

        // -- Init cube texture bindless set (binding 1, external set) --
        BCInitInfo ci_cube            = combined_ci_;
        ci_cube.binding               = static_cast<uint32_t>(ETextureSetBindings::CUBE_TEXTURES);
        ci_cube.view_type             = VK_IMAGE_VIEW_TYPE_CUBE;
        ci_cube.generate_mipmaps      = false;
        ci_cube.initial_capacity      = cube_max;
        ci_cube.layout_max_capacity   = cube_max;
        ci_cube.external_pool         = shared_pool_;
        ci_cube.external_set          = shared_set_;
        ci_cube.frames_in_flight      = slices_;
        if (!combined_cube_.init(ci_cube))
        { shutdown(); return false; }

        if (!initMipFeedback(slices_, tex2d_max))
        { shutdown(); return false; }

        // Fallback texture (2D)
        lux::rdesc::Texture fb = info.fallback_pixel.value_or(makeDefaultWhite());
        SlotHandle fallback = combined_.addTexture(fb, &default_sampler_ci_);
        // Flush immediately so the fallback texture is ready before any rendering
        combined_.flushUploads();

        fallback_bindless_index_ = fallback.index;
        noteTextureResident(fallback.index);

        return true;   // initialized_ was set at the top — see the note there
    }

    void TextureResources::shutdown()
    {
        if (!initialized_) return;
        initialized_ = false;

        shutdownMipFeedback();
        combined_.shutdown();
        combined_cube_.shutdown();

        // Destroy shared pool (after BindlessCombinedSet instances release references)
        if (shared_pool_ && dc_)
        {
            auto& device = dc_->logicalDevice();
            if (shared_set_)
            {
                vkFreeDescriptorSets(device, shared_pool_, 1, &shared_set_);
                shared_set_ = VK_NULL_HANDLE;
            }
            vkDestroyDescriptorPool(device, shared_pool_, nullptr);
            shared_pool_ = VK_NULL_HANDLE;
        }
    }

    Expected<TextureHandle>
    TextureResources::submit(const lux::rdesc::Texture &cpu,
        const VkSamplerCreateInfo *opt_sampler,
        VkFormat fmt,
        bool generate_mips)
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        SlotHandle sh = combined_.addTexture(cpu, &sci, fmt, generate_mips);
        if (!sh.isValid())
            return renderFailure<err::memory::CapacityExhausted>();
        TextureHandle h{sh.index, sh.gen};
        noteTextureResident(sh.index);
        return h;
    }

    namespace
    {
        // EPixelFormat → VkFormat for the UNCOMPRESSED formats the persistent path
        // accepts (create refused UnsupportedFormat before reaching this).
        [[nodiscard]] VkFormat persistentVkFormat(EPixelFormat f) noexcept
        {
            switch (f)
            {
            case EPixelFormat::RGBA8_SRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
            case EPixelFormat::RGBA8_UNORM:   return VK_FORMAT_R8G8B8A8_UNORM;
            case EPixelFormat::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case EPixelFormat::RG8_UNORM:     return VK_FORMAT_R8G8_UNORM;
            case EPixelFormat::R8_UNORM:      return VK_FORMAT_R8_UNORM;
            case EPixelFormat::R16_UINT:      return VK_FORMAT_R16_UINT;
            case EPixelFormat::R16_UNORM:     return VK_FORMAT_R16_UNORM;
            default:                          return VK_FORMAT_UNDEFINED;
            }
        }
    }

    Expected<TextureHandle> TextureResources::createPersistentTexture2D(
        const PersistentTexture2DDesc& desc,
        const VkSamplerCreateInfo* opt_sampler)
    {
        // 与客户端预检共用的那个纯函数校验器。
        const auto validation = validatePersistentTexture2DDesc(desc);
        if (!validation.ok())
        {
            if (validation.status == ERegionUploadStatus::UnsupportedFormat)
                return renderFailure<err::asset::UnsupportedFormat>();
            return renderFailure<err::internal::InvalidArgument>();
        }

        // 实现限制而非协议限制:2D bindless set 建的是 VK_IMAGE_VIEW_TYPE_2D 视图,
        // 分层的持久纹理走的是 2D_ARRAY set 上的 chunk 图集切片。
        if (desc.array_layers != 1)
            return renderFailure<err::internal::InvalidArgument>();

        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;
        const SlotHandle sh = combined_.addPersistentTexture(
            desc.width, desc.height, desc.mip_levels, persistentVkFormat(desc.format), &sci);
        if (!sh.isValid())
            return renderFailure<err::memory::CapacityExhausted>();

        persistent_descs_.emplace(sh.index, desc);
        noteTextureResident(sh.index);
        return TextureHandle{sh.index, sh.gen};
    }

    ERegionUploadStatus TextureResources::updateTextureRegions(
        TextureHandle h,
        std::span<const TextureRegionDesc> regions,
        std::span<const std::byte> pixels)
    {
        const SlotHandle sh{h.index, h.gen};
        if (!combined_.isTextureAlive(sh))
            return ERegionUploadStatus::InvalidHandle;
        const auto it = persistent_descs_.find(h.index);
        if (it == persistent_descs_.end())
            return ERegionUploadStatus::InvalidHandle;   // immutable asset texture — not updatable

        // Authoritative bounds check — the SAME pure validator the client used.
        if (const auto v = validateTextureRegions(it->second, regions, pixels.size()); !v.ok())
            return v.status;

        // Wire descs → the bindless set's comm-free mirror (identical fields).
        std::vector<BindlessCombinedSet::RegionUpdate> updates;
        updates.reserve(regions.size());
        for (const TextureRegionDesc& r : regions)
            updates.push_back(BindlessCombinedSet::RegionUpdate{
                r.x, r.y, r.width, r.height, r.mip, r.array_layer,
                r.row_pitch_bytes, r.data_offset});

        return combined_.updateTextureRegions(
                   sh, updates, pixels, regionTexelBytes(it->second.format))
                   ? ERegionUploadStatus::Ok
                   : ERegionUploadStatus::InvalidHandle;
    }

    bool TextureResources::remove(TextureHandle h)
    {
        persistent_descs_.erase(h.index);   // no-op for immutable asset textures
        const bool removed = combined_.removeTexture(SlotHandle{h.index, h.gen});
        if (removed && h.index < mip_states_.size())
        {
            clearMipDemand(h.index);
            mip_states_[h.index] = {};
        }
        return removed;
    }

    bool TextureResources::removeCube(TextureHandle h)
    {
        return combined_cube_.removeTexture(SlotHandle{h.index, h.gen});
    }

    Expected<TextureHandle>
    TextureResources::submitCube(
        const lux::rdesc::Texture faces[6],
        const VkSamplerCreateInfo *opt_sampler,
        VkFormat fmt)
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        SlotHandle sh = combined_cube_.addCubeTexture(faces, &sci, fmt);
        if (!sh.isValid())
            return renderFailure<err::memory::CapacityExhausted>();
        TextureHandle h{sh.index, sh.gen};
        return h;
    }

    bool TextureResources::initMipFeedback(
        std::uint32_t frames_in_flight,
        std::uint32_t capacity)
    {
        mip_feedback_capacity_ = std::max(capacity, 1u);
        mip_states_.assign(mip_feedback_capacity_, {});
        mip_feedback_frames_.assign(
            std::max(frames_in_flight, 1u), {});
        // One extra word is the workgroup aggregation fallback counter. It is
        // deliberately outside the public texture-slot capacity so material
        // indices can never address it as a wanted-mip entry.
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(
            mip_feedback_capacity_ + 1u) * sizeof(std::uint32_t);
        for (auto& frame : mip_feedback_frames_)
        {
            void* mapped = nullptr;
            if (!createGpuBufferVmaBuffer(
                    dc_->vmaAllocator(),
                    bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    true,
                    &frame.buffer,
                    &frame.allocation,
                    &mapped) ||
                frame.buffer == VK_NULL_HANDLE || mapped == nullptr)
            {
                shutdownMipFeedback();
                return false;
            }
            frame.mapped = static_cast<std::uint32_t*>(mapped);
            std::fill_n(
                frame.mapped,
                mip_feedback_capacity_,
                std::numeric_limits<std::uint32_t>::max());
            frame.mapped[mip_feedback_capacity_] = 0u;
            flushGpuBufferVmaAllocation(
                dc_->vmaAllocator(), frame.allocation, 0u, bytes);
        }
        return true;
    }

    void TextureResources::shutdownMipFeedback() noexcept
    {
        for (auto& frame : mip_feedback_frames_)
        {
            if (frame.buffer != VK_NULL_HANDLE)
            {
                if (deferred_queue_)
                    deferred_queue_->retireBuffer(
                        frame.buffer, frame.allocation);
                else if (dc_)
                    destroyGpuBufferVmaBuffer(
                        dc_->vmaAllocator(),
                        frame.buffer,
                        frame.allocation);
            }
            frame = {};
        }
        mip_feedback_frames_.clear();
        mip_states_.clear();
        mip_demand_slots_.clear();
        mip_feedback_capacity_ = 0u;
        mip_feedback_snapshot_ = {};
    }

    void TextureResources::noteTextureResident(
        std::uint32_t slot_index,
        std::uint32_t logical_base_mip) noexcept
    {
        if (slot_index >= mip_states_.size())
            return;
        auto& state = mip_states_[slot_index];
        const VkFormat physical_format = combined_.slotFormat(slot_index);
        const std::uint32_t physical_width = combined_.slotWidth(slot_index);
        const std::uint32_t physical_height = combined_.slotHeight(slot_index);
        const std::uint32_t physical_mips = combined_.slotMipLevels(slot_index);
        if (!state.alive)
        {
            // Initial creation defines the logical shape. A replacement cannot
            // establish a new resource identity at a non-zero logical base.
            if (logical_base_mip != 0u)
                return;
            state.format = physical_format;
            state.width = physical_width;
            state.height = physical_height;
            state.total_mips = physical_mips;
            state.target_base_mip = 0u;
        }
        state.resident_base_mip = logical_base_mip;
        state.no_demand_frames = 0u;
        state.replacement_pending = false;
        state.alive = state.total_mips != 0u && state.width != 0u &&
            state.height != 0u && physical_mips != 0u &&
            physical_width != 0u && physical_height != 0u &&
            physical_format == state.format;
        if (state.alive &&
            state.target_base_mip != state.resident_base_mip)
        {
            markMipDemand(slot_index);
        }
        else
        {
            clearMipDemand(slot_index);
        }
    }

    namespace
    {
        [[nodiscard]] VkFormat immutableTextureVkFormat(
            EPixelFormat format) noexcept
        {
            switch (format)
            {
            case EPixelFormat::RGBA8_SRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
            case EPixelFormat::RGBA8_UNORM:   return VK_FORMAT_R8G8B8A8_UNORM;
            case EPixelFormat::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case EPixelFormat::RG8_UNORM:     return VK_FORMAT_R8G8_UNORM;
            case EPixelFormat::R8_UNORM:      return VK_FORMAT_R8_UNORM;
            case EPixelFormat::BC1_SRGB:      return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case EPixelFormat::BC3_SRGB:      return VK_FORMAT_BC3_SRGB_BLOCK;
            case EPixelFormat::BC5_UNORM:     return VK_FORMAT_BC5_UNORM_BLOCK;
            case EPixelFormat::BC7_SRGB:      return VK_FORMAT_BC7_SRGB_BLOCK;
            case EPixelFormat::R16_UINT:      return VK_FORMAT_R16_UINT;
            case EPixelFormat::R16_UNORM:     return VK_FORMAT_R16_UNORM;
            }
            return VK_FORMAT_UNDEFINED;
        }
    }

    bool TextureResources::beginMipReplacement(
        TextureHandle handle,
        EPixelFormat format,
        std::uint32_t logical_base_mip,
        std::uint32_t physical_width,
        std::uint32_t physical_height,
        std::uint32_t physical_mip_count) noexcept
    {
        const SlotHandle slot{handle.index, handle.gen};
        if (!combined_.isTextureAlive(slot) ||
            handle.index >= mip_states_.size())
        {
            return false;
        }
        auto& state = mip_states_[handle.index];
        if (!state.alive || state.replacement_pending ||
            logical_base_mip >= state.total_mips ||
            immutableTextureVkFormat(format) != state.format ||
            physical_width != std::max(state.width >> logical_base_mip, 1u) ||
            physical_height != std::max(state.height >> logical_base_mip, 1u) ||
            physical_mip_count == 0u ||
            physical_mip_count > state.total_mips - logical_base_mip)
        {
            return false;
        }
        state.replacement_pending = true;
        return true;
    }

    void TextureResources::endMipReplacement(TextureHandle handle) noexcept
    {
        if (handle.index >= mip_states_.size())
            return;
        auto& state = mip_states_[handle.index];
        if (combined_.isTextureAlive(SlotHandle{handle.index, handle.gen}))
        {
            state.replacement_pending = false;
            if (state.alive &&
                state.target_base_mip != state.resident_base_mip)
            {
                markMipDemand(handle.index);
            }
        }
    }

    void TextureResources::markMipDemand(
        std::uint32_t slot_index) noexcept
    {
        if (slot_index >= mip_states_.size())
            return;
        auto& state = mip_states_[slot_index];
        if (state.demand_index !=
            std::numeric_limits<std::uint32_t>::max())
        {
            return;
        }
        state.demand_index = static_cast<std::uint32_t>(
            mip_demand_slots_.size());
        mip_demand_slots_.push_back(slot_index);
    }

    void TextureResources::clearMipDemand(
        std::uint32_t slot_index) noexcept
    {
        if (slot_index >= mip_states_.size())
            return;
        auto& state = mip_states_[slot_index];
        const std::uint32_t index = state.demand_index;
        if (index == std::numeric_limits<std::uint32_t>::max())
            return;
        const std::uint32_t moved = mip_demand_slots_.back();
        mip_demand_slots_[index] = moved;
        mip_states_[moved].demand_index = index;
        mip_demand_slots_.pop_back();
        state.demand_index = std::numeric_limits<std::uint32_t>::max();
    }

    TextureMipDemandsReply TextureResources::mipDemands(
        std::uint32_t maximum_count) const noexcept
    {
        TextureMipDemandsReply reply{};
        const std::uint32_t limit = std::min(
            maximum_count,
            kTextureMipDemandBatchCapacity);
        for (const std::uint32_t slot : mip_demand_slots_)
        {
            if (slot >= mip_states_.size())
                continue;
            const auto& state = mip_states_[slot];
            if (!state.alive || state.replacement_pending ||
                state.target_base_mip == state.resident_base_mip)
            {
                continue;
            }
            if (reply.count < limit)
            {
                reply.entries[reply.count++] = TextureMipDemandEntry{
                    RTextureHandle{slot, combined_.genAt(slot)},
                    state.resident_base_mip,
                    state.target_base_mip};
            }
            else
            {
                ++reply.remaining_count;
            }
        }
        return reply;
    }

    VkDeviceAddress TextureResources::mipFeedbackAddress(
        std::uint32_t frame_slot) const noexcept
    {
        if (!dc_ || mip_feedback_frames_.empty())
            return 0u;
        const auto& frame = mip_feedback_frames_[
            frame_slot % mip_feedback_frames_.size()];
        if (frame.buffer == VK_NULL_HANDLE)
            return 0u;
        VkBufferDeviceAddressInfo info{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = frame.buffer;
        return vkGetBufferDeviceAddress(dc_->logicalDevice(), &info);
    }

    std::uint64_t TextureResources::mipBytes(
        const TextureMipState& state,
        std::uint32_t base_mip) const noexcept
    {
        std::uint64_t bytes = 0u;
        for (std::uint32_t mip = std::min(base_mip, state.total_mips);
             mip < state.total_mips;
             ++mip)
        {
            bytes += vkFormatMipBytes(
                state.format,
                std::max(state.width >> mip, 1u),
                std::max(state.height >> mip, 1u));
        }
        return bytes;
    }

    void TextureResources::adoptMipFeedback(
        const FrameStamp& stamp) noexcept
    {
        if (mip_feedback_frames_.empty() || mip_feedback_capacity_ == 0u)
            return;
        auto& frame = mip_feedback_frames_[
            stamp.slotIndex() % mip_feedback_frames_.size()];
        const bool sample = frame.last_submit_serial != 0u &&
            (stamp.serial % 4u) == 0u;
        if (sample && frame.mapped)
        {
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(
                mip_feedback_capacity_ + 1u) * sizeof(std::uint32_t);
            invalidateGpuBufferVmaAllocation(
                dc_->vmaAllocator(), frame.allocation, 0u, bytes);

            TextureMipFeedbackSnapshot next{};
            next.sample_serial = stamp.serial;
            next.minimum_wanted_mip =
                std::numeric_limits<std::uint32_t>::max();
            next.aggregation_fallback_count =
                frame.mapped[mip_feedback_capacity_];
            next.valid = 1u;
            for (std::uint32_t slot = 0u;
                 slot < mip_feedback_capacity_;
                 ++slot)
            {
                auto& state = mip_states_[slot];
                if (!state.alive)
                    continue;
                const std::uint32_t wanted = frame.mapped[slot];
                if (wanted != std::numeric_limits<std::uint32_t>::max())
                {
                    const std::uint32_t clamped = std::min(
                        wanted, state.total_mips - 1u);
                    ++next.sampled_texture_count;
                    next.minimum_wanted_mip = std::min(
                        next.minimum_wanted_mip, clamped);
                    if (clamped < state.target_base_mip)
                    {
                        state.target_base_mip = clamped;
                        state.no_demand_frames = 0u;
                        ++next.upgrade_request_count;
                    }
                    else if (clamped > state.target_base_mip)
                    {
                        state.no_demand_frames = std::min<std::uint32_t>(
                            state.no_demand_frames + 4u,
                            120u);
                        if (state.no_demand_frames >= 120u)
                        {
                            ++state.target_base_mip;
                            state.no_demand_frames = 0u;
                            ++next.downgrade_request_count;
                        }
                    }
                    else
                    {
                        state.no_demand_frames = 0u;
                    }
                }
                else
                {
                    state.no_demand_frames = std::min<std::uint32_t>(
                        state.no_demand_frames + 4u,
                        120u);
                    if (state.no_demand_frames >= 120u &&
                        state.target_base_mip + 1u < state.total_mips)
                    {
                        ++state.target_base_mip;
                        state.no_demand_frames = 0u;
                        ++next.downgrade_request_count;
                    }
                }
                next.full_resident_bytes += mipBytes(state, 0u);
                next.target_resident_bytes += mipBytes(
                    state, state.target_base_mip);
                next.actual_resident_bytes += mipBytes(
                    state, state.resident_base_mip);
                if (state.target_base_mip != state.resident_base_mip)
                    markMipDemand(slot);
                else
                    clearMipDemand(slot);
            }
            mip_feedback_snapshot_ = next;
            std::fill_n(
                frame.mapped,
                mip_feedback_capacity_,
                std::numeric_limits<std::uint32_t>::max());
            frame.mapped[mip_feedback_capacity_] = 0u;
            flushGpuBufferVmaAllocation(
                dc_->vmaAllocator(), frame.allocation, 0u, bytes);
        }
        // The buffer belongs to a fence-safe FIF slot at this point. Mark the
        // serial that will consume it; a later reuse can adopt all atomicMin
        // operations without a CPU/GPU race.
        frame.last_submit_serial = stamp.serial;
    }

    lux::rdesc::Texture TextureResources::makeDefaultWhite()
    {
        static uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};

        lux::rdesc::TextureInfo info{};
        info.width = 1;
        info.height = 1;
        info.channel = 4;
        info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        info.color_space = lux::rdesc::ETextureColorSpace::LINEAR;
        info.layers = 1;
        info.mip_count = 1;
        info.mip_ranges[0] = {
            .offset = 0,
            .size = 4,
            .width = 1,
            .height = 1,
        };

        auto texture = lux::rdesc::Texture::copyOf(
            info,
            std::as_bytes(std::span{white}));
        if (!texture)
            return {};
        return std::move(*texture);
    }

}
