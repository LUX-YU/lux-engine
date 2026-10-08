#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{
    struct TextureResources::Backing
    {
        struct FeedbackFrame
        {
            VmaBuffer buffer;
            std::uint32_t* mapped{};
        };

        DescriptorPoolOwner pool;
        VkDescriptorSet set{};
        std::unique_ptr<BindlessCombinedSet> textures;
        std::unique_ptr<BindlessCombinedSet> cubes;
        std::vector<FeedbackFrame> feedback;
        VkSamplerCreateInfo sampler{};
        std::uint32_t fallback_index{};
    };

    TextureResources::CreateResult TextureResources::create(const CreateInfo& info) noexcept
    {
        const auto& config = info.combined_ci;
        const bool is_missing_binding =
            !config.resource_context || !config.deferred_queue || !config.descriptor_set_layout;
        const bool is_invalid_capacity =
            config.layout_max_capacity == 0 || info.cube_max_capacity == 0 ||
            config.layout_max_capacity > std::numeric_limits<std::uint32_t>::max() - info.cube_max_capacity;
        const bool is_invalid_configuration = is_missing_binding || is_invalid_capacity || config.frames_in_flight == 0;
        if (is_invalid_configuration)
        {
            return renderFailure<err::memory::InvalidTextureConfiguration>();
        }

        auto& resources = *config.resource_context;
        const VkDevice device = resources.logicalDevice();
        Backing backing;
        VkDescriptorPoolSize size{
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            config.layout_max_capacity + info.cube_max_capacity
        };
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags =
            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        auto pool = DescriptorPoolOwner::create(device, pool_info);
        if (!pool)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(pool.error()));
        }
        backing.pool = std::move(*pool);

        // Counts come from the actual layout. Only the highest binding (cube) is variable.
        VkDescriptorSetVariableDescriptorCountAllocateInfo counts{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
        };
        counts.descriptorSetCount = 1;
        counts.pDescriptorCounts = &info.cube_max_capacity;
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = backing.pool.get();
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &config.descriptor_set_layout;
        allocation.pNext = &counts;
        VK_EXPECT(vkAllocateDescriptorSets(device, &allocation, &backing.set));

        auto textures = config;
        textures.external_pool = backing.pool.get();
        textures.external_set = backing.set;
        textures.initial_capacity = config.layout_max_capacity;
        auto texture_set = BindlessCombinedSet::create(textures);
        if (!texture_set)
        {
            return lux::cxx::unexpected(texture_set.error());
        }
        backing.textures = std::move(*texture_set);

        auto cubes = textures;
        cubes.binding = static_cast<std::uint32_t>(ETextureSetBindings::CUBE_TEXTURES);
        cubes.view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        cubes.generate_mipmaps = false;
        cubes.initial_capacity = cubes.layout_max_capacity = info.cube_max_capacity;
        auto cube_set = BindlessCombinedSet::create(cubes);
        if (!cube_set)
        {
            return lux::cxx::unexpected(cube_set.error());
        }
        backing.cubes = std::move(*cube_set);

        const VkDeviceSize bytes = (static_cast<VkDeviceSize>(config.layout_max_capacity) + 1) * sizeof(std::uint32_t);
        backing.feedback.reserve(config.frames_in_flight);
        for (std::uint32_t index = 0; index < config.frames_in_flight; ++index)
        {
            VkBuffer buffer{};
            VmaAllocation buffer_allocation{};
            void* mapped{};
            const auto status = createGpuBufferVmaBuffer(
                resources.vmaAllocator(),
                bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                true,
                &buffer,
                &buffer_allocation,
                &mapped
            );
            if (status != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
            }
            auto owner = VmaBuffer::adopt({resources.vmaAllocator(), buffer, buffer_allocation});
            if (!mapped)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            auto* words = static_cast<std::uint32_t*>(mapped);
            std::fill_n(words, config.layout_max_capacity, std::numeric_limits<std::uint32_t>::max());
            words[config.layout_max_capacity] = 0;
            VK_EXPECT(vmaFlushAllocation(resources.vmaAllocator(), buffer_allocation, 0, bytes));
            backing.feedback.push_back({std::move(owner), words});
        }

        backing.sampler = info.default_sampler_ci;
        if (backing.sampler.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)
        {
            backing.sampler = {};
            backing.sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            backing.sampler.magFilter = backing.sampler.minFilter = VK_FILTER_LINEAR;
            backing.sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            backing.sampler.addressModeU = backing.sampler.addressModeV = backing.sampler.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_REPEAT;
            backing.sampler.maxLod = VK_LOD_CLAMP_NONE;
        }
        const auto fallback = info.fallback_pixel.value_or(makeDefaultWhite());
        auto slot = backing.textures->addTexture(fallback, &backing.sampler);
        if (!slot)
        {
            return lux::cxx::unexpected(slot.error());
        }
        auto uploaded = backing.textures->flushUploads();
        if (!uploaded)
        {
            return lux::cxx::unexpected(uploaded.error());
        }
        backing.fallback_index = slot->index;
        return std::unique_ptr<TextureResources>(new TextureResources(info, std::move(backing)));
    }

    TextureResources::TextureResources(const CreateInfo& info, Backing&& backing) noexcept
        : dc_(info.combined_ci.resource_context->deviceContext()), shared_pool_(std::move(backing.pool)),
          combined_(std::move(backing.textures)), combined_cube_(std::move(backing.cubes)),
          default_sampler_ci_(backing.sampler), fallback_bindless_index_(backing.fallback_index),
          mip_states_(info.combined_ci.layout_max_capacity),
          mip_feedback_capacity_(info.combined_ci.layout_max_capacity)
    {
        mip_feedback_frames_.reserve(backing.feedback.size());
        for (auto& frame : backing.feedback)
        {
            const auto allocation = frame.buffer.release();
            TFifOwnedAllocated<VkBuffer> buffer(
                *info.combined_ci.deferred_queue,
                allocation.buffer,
                allocation.allocation
            );
            mip_feedback_frames_.push_back({std::move(buffer), frame.mapped, 0});
        }
        noteTextureResident(fallback_bindless_index_);
    }

    TextureResources::~TextureResources() noexcept = default;

    Expected<TextureHandle> TextureResources::submit(
        const lux::rdesc::Texture& cpu,
        const VkSamplerCreateInfo* opt_sampler,
        VkFormat fmt,
        bool generate_mips
    )
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        auto sh = combined_->addTexture(cpu, &sci, fmt, generate_mips);
        if (!sh)
        {
            return lux::cxx::unexpected(sh.error());
        }
        TextureHandle h{sh->index, sh->gen};
        noteTextureResident(sh->index);
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
            case EPixelFormat::RGBA8_SRGB:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case EPixelFormat::RGBA8_UNORM:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case EPixelFormat::RGBA16_SFLOAT:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case EPixelFormat::RG8_UNORM:
                return VK_FORMAT_R8G8_UNORM;
            case EPixelFormat::R8_UNORM:
                return VK_FORMAT_R8_UNORM;
            case EPixelFormat::R16_UINT:
                return VK_FORMAT_R16_UINT;
            case EPixelFormat::R16_UNORM:
                return VK_FORMAT_R16_UNORM;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }
    }

    Expected<TextureHandle> TextureResources::createPersistentTexture2D(
        const PersistentTexture2DDesc& desc,
        const VkSamplerCreateInfo* opt_sampler
    )
    {
        // 与客户端预检共用的那个纯函数校验器。
        const auto validation = validatePersistentTexture2DDesc(desc);
        if (!validation.ok())
        {
            if (validation.status == ERegionUploadStatus::UNSUPPORTED_FORMAT)
                return renderFailure<err::asset::UnsupportedFormat>();
            return renderFailure<err::internal::InvalidArgument>();
        }

        // 实现限制而非协议限制:2D bindless set 建的是 VK_IMAGE_VIEW_TYPE_2D 视图,
        // 分层的持久纹理走的是 2D_ARRAY set 上的 chunk 图集切片。
        if (desc.array_layers != 1)
            return renderFailure<err::internal::InvalidArgument>();

        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;
        const auto format = persistentVkFormat(desc.format);
        const auto sh = combined_->addPersistentTexture(desc.width, desc.height, desc.mip_levels, format, &sci);
        if (!sh)
        {
            return lux::cxx::unexpected(sh.error());
        }

        persistent_descs_.emplace(sh->index, desc);
        noteTextureResident(sh->index);
        return TextureHandle{sh->index, sh->gen};
    }

    ERegionUploadStatus TextureResources::updateTextureRegions(
        TextureHandle h,
        std::span<const TextureRegionDesc> regions,
        std::span<const std::byte> pixels
    )
    {
        const SlotHandle sh{h.index, h.gen};
        if (!combined_->isTextureAlive(sh))
        {
            return ERegionUploadStatus::INVALID_HANDLE;
        }
        const auto it = persistent_descs_.find(h.index);
        if (it == persistent_descs_.end())
            return ERegionUploadStatus::INVALID_HANDLE; // immutable asset texture — not updatable

        // Authoritative bounds check — the SAME pure validator the client used.
        if (const auto v = validateTextureRegions(it->second, regions, pixels.size()); !v.ok())
            return v.status;

        // Wire descs → the bindless set's comm-free mirror (identical fields).
        std::vector<BindlessCombinedSet::RegionUpdate> updates;
        updates.reserve(regions.size());
        for (const TextureRegionDesc& r : regions)
            updates.push_back(BindlessCombinedSet::RegionUpdate{
                r.x,
                r.y,
                r.width,
                r.height,
                r.mip,
                r.array_layer,
                r.row_pitch_bytes,
                r.data_offset
            });

        return combined_->updateTextureRegions(sh, updates, pixels, regionTexelBytes(it->second.format))
                   ? ERegionUploadStatus::OK
                   : ERegionUploadStatus::INVALID_HANDLE;
    }

    bool TextureResources::remove(TextureHandle h)
    {
        const bool removed = combined_->removeTexture(SlotHandle{h.index, h.gen});
        if (removed)
        {
            unpublish(remoteTexture(h));
            persistent_descs_.erase(h.index);
        }
        if (removed && h.index < mip_states_.size())
        {
            clearMipDemand(h.index);
            mip_states_[h.index] = {};
        }
        return removed;
    }

    bool TextureResources::removeCube(TextureHandle h)
    {
        const bool removed = combined_cube_->removeTexture(SlotHandle{h.index, h.gen});
        if (removed)
            unpublish(remoteTexture(h, true));
        return removed;
    }

    RTextureHandle TextureResources::publishTexture(TextureHandle local, bool cube)
    {
        if (const auto existing = remoteTexture(local, cube); existing.isValid())
            return existing;
        auto& reverse = cube ? remote_cube_ : remote_2d_;
        if (reverse.size() <= local.index)
            reverse.resize(std::size_t(local.index) + 1);
        const auto key =
            remote_textures_.emplace(RemoteTexture{cube ? ERemoteKind::CUBE : ERemoteKind::TEXTURE_2D, local, {}});
        return reverse[local.index] = RTextureHandle{key.index, key.gen};
    }

    RTextureHandle TextureResources::publishOutput(RenderTargetId target)
    {
        const auto key = remote_textures_.emplace(RemoteTexture{ERemoteKind::OUTPUT, {}, target});
        return {key.index, key.gen};
    }

    const TextureResources::RemoteTexture* TextureResources::resolve(RTextureHandle remote) const noexcept
    {
        return remote_textures_.find({remote.index, remote.gen});
    }

    TextureHandle TextureResources::resolveTexture(RTextureHandle remote, bool cube) const noexcept
    {
        const auto* entry = resolve(remote);
        if (!entry || entry->kind != (cube ? ERemoteKind::CUBE : ERemoteKind::TEXTURE_2D))
            return {};
        const auto& set = cube ? *combined_cube_ : *combined_;
        return set.isTextureAlive({entry->local.index, entry->local.gen}) ? entry->local : TextureHandle{};
    }

    RTextureHandle TextureResources::remoteTexture(TextureHandle local, bool cube) const noexcept
    {
        const auto& reverse = cube ? remote_cube_ : remote_2d_;
        if (local.index >= reverse.size())
            return {};
        const auto remote = reverse[local.index];
        const auto* entry = resolve(remote);
        return entry && entry->local == local ? remote : RTextureHandle{};
    }

    void TextureResources::unpublish(RTextureHandle remote) noexcept
    {
        const auto* entry = resolve(remote);
        if (!entry)
            return;
        if (entry->kind != ERemoteKind::OUTPUT)
        {
            auto& reverse = entry->kind == ERemoteKind::CUBE ? remote_cube_ : remote_2d_;
            reverse[entry->local.index] = {};
        }
        remote_textures_.erase({remote.index, remote.gen});
    }

    Expected<TextureHandle> TextureResources::submitCube(
        const lux::rdesc::Texture faces[6],
        const VkSamplerCreateInfo* opt_sampler,
        VkFormat fmt
    )
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        auto sh = combined_cube_->addCubeTexture(faces, &sci, fmt);
        if (!sh)
        {
            return lux::cxx::unexpected(sh.error());
        }
        TextureHandle h{sh->index, sh->gen};
        return h;
    }

    void TextureResources::noteTextureResident(std::uint32_t slot_index, std::uint32_t logical_base_mip) noexcept
    {
        if (slot_index >= mip_states_.size())
            return;
        auto& state = mip_states_[slot_index];
        const VkFormat physical_format = combined_->slotFormat(slot_index);
        const std::uint32_t physical_width = combined_->slotWidth(slot_index);
        const std::uint32_t physical_height = combined_->slotHeight(slot_index);
        const std::uint32_t physical_mips = combined_->slotMipLevels(slot_index);
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
        state.alive = state.total_mips != 0u && state.width != 0u && state.height != 0u && physical_mips != 0u &&
                      physical_width != 0u && physical_height != 0u && physical_format == state.format;
        if (state.alive && state.target_base_mip != state.resident_base_mip)
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
        [[nodiscard]] VkFormat immutableTextureVkFormat(EPixelFormat format) noexcept
        {
            switch (format)
            {
            case EPixelFormat::RGBA8_SRGB:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case EPixelFormat::RGBA8_UNORM:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case EPixelFormat::RGBA16_SFLOAT:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case EPixelFormat::RG8_UNORM:
                return VK_FORMAT_R8G8_UNORM;
            case EPixelFormat::R8_UNORM:
                return VK_FORMAT_R8_UNORM;
            case EPixelFormat::BC1_SRGB:
                return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case EPixelFormat::BC3_SRGB:
                return VK_FORMAT_BC3_SRGB_BLOCK;
            case EPixelFormat::BC5_UNORM:
                return VK_FORMAT_BC5_UNORM_BLOCK;
            case EPixelFormat::BC7_SRGB:
                return VK_FORMAT_BC7_SRGB_BLOCK;
            case EPixelFormat::R16_UINT:
                return VK_FORMAT_R16_UINT;
            case EPixelFormat::R16_UNORM:
                return VK_FORMAT_R16_UNORM;
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
        std::uint32_t physical_mip_count
    ) noexcept
    {
        const SlotHandle slot{handle.index, handle.gen};
        if (!combined_->isTextureAlive(slot) || handle.index >= mip_states_.size())
        {
            return false;
        }
        auto& state = mip_states_[handle.index];
        const bool is_invalid_state = !state.alive || state.replacement_pending;
        const bool has_valid_base_mip = logical_base_mip < state.total_mips;
        const bool is_invalid_base_mip = !has_valid_base_mip || immutableTextureVkFormat(format) != state.format;
        const bool is_invalid_dimensions = !has_valid_base_mip ||
                                           physical_width != std::max(state.width >> logical_base_mip, 1u) ||
                                           physical_height != std::max(state.height >> logical_base_mip, 1u);
        const bool is_invalid_mip_count =
            !has_valid_base_mip || physical_mip_count == 0u || physical_mip_count > state.total_mips - logical_base_mip;
        const bool is_invalid_request =
            is_invalid_state || is_invalid_base_mip || is_invalid_dimensions || is_invalid_mip_count;
        if (is_invalid_request)
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
        if (combined_->isTextureAlive(SlotHandle{handle.index, handle.gen}))
        {
            state.replacement_pending = false;
            if (state.alive && state.target_base_mip != state.resident_base_mip)
            {
                markMipDemand(handle.index);
            }
        }
    }

    void TextureResources::markMipDemand(std::uint32_t slot_index) noexcept
    {
        if (slot_index >= mip_states_.size())
            return;
        auto& state = mip_states_[slot_index];
        if (state.demand_index != std::numeric_limits<std::uint32_t>::max())
        {
            return;
        }
        state.demand_index = static_cast<std::uint32_t>(mip_demand_slots_.size());
        mip_demand_slots_.push_back(slot_index);
    }

    void TextureResources::clearMipDemand(std::uint32_t slot_index) noexcept
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

    TextureMipDemandsReply TextureResources::mipDemands(std::uint32_t maximum_count) const noexcept
    {
        TextureMipDemandsReply reply{};
        const std::uint32_t limit = std::min(maximum_count, kTextureMipDemandBatchCapacity);
        for (const std::uint32_t slot : mip_demand_slots_)
        {
            if (slot >= mip_states_.size())
                continue;
            const auto& state = mip_states_[slot];
            if (!state.alive || state.replacement_pending || state.target_base_mip == state.resident_base_mip)
            {
                continue;
            }
            const auto texture = remoteTexture(TextureHandle{slot, combined_->genAt(slot)});
            if (!texture.isValid())
                continue;
            if (reply.count < limit)
            {
                reply.entries[reply.count++] =
                    TextureMipDemandEntry{texture, state.resident_base_mip, state.target_base_mip};
            }
            else
            {
                ++reply.remaining_count;
            }
        }
        return reply;
    }

    VkDeviceAddress TextureResources::mipFeedbackAddress(std::uint32_t frame_slot) const noexcept
    {
        const auto& frame = mip_feedback_frames_[frame_slot % mip_feedback_frames_.size()];
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = frame.buffer.get();
        return vkGetBufferDeviceAddress(dc_.logicalDevice(), &info);
    }

    std::uint64_t TextureResources::mipBytes(const TextureMipState& state, std::uint32_t base_mip) const noexcept
    {
        std::uint64_t bytes = 0u;
        for (std::uint32_t mip = std::min(base_mip, state.total_mips); mip < state.total_mips; ++mip)
        {
            bytes +=
                vkFormatMipBytes(state.format, std::max(state.width >> mip, 1u), std::max(state.height >> mip, 1u));
        }
        return bytes;
    }

    void TextureResources::adoptMipFeedback(const FrameStamp& stamp) noexcept
    {
        auto& frame = mip_feedback_frames_[stamp.slotIndex() % mip_feedback_frames_.size()];
        const bool sample = frame.last_submit_serial != 0u && (stamp.serial % 4u) == 0u;
        if (sample)
        {
            const VkDeviceSize bytes = (static_cast<VkDeviceSize>(mip_feedback_capacity_) + 1u) * sizeof(std::uint32_t);
            invalidateGpuBufferVmaAllocation(dc_.vmaAllocator(), frame.buffer.alloc(), 0u, bytes);

            TextureMipFeedbackSnapshot next{};
            next.sample_serial = stamp.serial;
            next.minimum_wanted_mip = std::numeric_limits<std::uint32_t>::max();
            next.aggregation_fallback_count = frame.mapped[mip_feedback_capacity_];
            next.valid = 1u;
            for (std::uint32_t slot = 0u; slot < mip_feedback_capacity_; ++slot)
            {
                auto& state = mip_states_[slot];
                if (!state.alive)
                    continue;
                const std::uint32_t wanted = frame.mapped[slot];
                if (wanted != std::numeric_limits<std::uint32_t>::max())
                {
                    const std::uint32_t clamped = std::min(wanted, state.total_mips - 1u);
                    ++next.sampled_texture_count;
                    next.minimum_wanted_mip = std::min(next.minimum_wanted_mip, clamped);
                    if (clamped < state.target_base_mip)
                    {
                        state.target_base_mip = clamped;
                        state.no_demand_frames = 0u;
                        ++next.upgrade_request_count;
                    }
                    else if (clamped > state.target_base_mip)
                    {
                        state.no_demand_frames = std::min<std::uint32_t>(state.no_demand_frames + 4u, 120u);
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
                    state.no_demand_frames = std::min<std::uint32_t>(state.no_demand_frames + 4u, 120u);
                    if (state.no_demand_frames >= 120u && state.target_base_mip + 1u < state.total_mips)
                    {
                        ++state.target_base_mip;
                        state.no_demand_frames = 0u;
                        ++next.downgrade_request_count;
                    }
                }
                next.full_resident_bytes += mipBytes(state, 0u);
                next.target_resident_bytes += mipBytes(state, state.target_base_mip);
                next.actual_resident_bytes += mipBytes(state, state.resident_base_mip);
                if (state.target_base_mip != state.resident_base_mip)
                    markMipDemand(slot);
                else
                    clearMipDemand(slot);
            }
            mip_feedback_snapshot_ = next;
            std::fill_n(frame.mapped, mip_feedback_capacity_, std::numeric_limits<std::uint32_t>::max());
            frame.mapped[mip_feedback_capacity_] = 0u;
            flushGpuBufferVmaAllocation(dc_.vmaAllocator(), frame.buffer.alloc(), 0u, bytes);
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

        auto texture = lux::rdesc::Texture::copyOf(info, std::as_bytes(std::span{white}));
        if (!texture)
            return {};
        return std::move(*texture);
    }

}
