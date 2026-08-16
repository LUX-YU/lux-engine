#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace lux::render
{
    namespace
    {
        constexpr std::uint32_t kHeightCount =
            kTerrainWireSampleEdge * kTerrainWireSampleEdge;
        constexpr std::uint32_t kWeightBytes = kHeightCount * 4u;
        constexpr std::uint32_t kHoleBytes = (kHeightCount + 7u) / 8u;
        constexpr std::uint32_t kFallbackEdge =
            (kTerrainWireQuadEdge / 2u) + 1u;
        constexpr std::uint32_t kFallbackCount =
            kFallbackEdge * kFallbackEdge;
        constexpr std::uint32_t kInvalidSlot = 0xffffffffu;

        [[nodiscard]] constexpr std::size_t minMaxOffset() noexcept
        {
            return sizeof(TerrainWirePageDataHeader) +
                static_cast<std::size_t>(kHeightCount) *
                    sizeof(std::uint16_t) +
                static_cast<std::size_t>(kWeightBytes) * 2u +
                kHoleBytes;
        }

        [[nodiscard]] constexpr std::size_t align16(
            std::size_t value) noexcept
        {
            return (value + 15u) & ~std::size_t{15u};
        }

        [[nodiscard]] constexpr std::size_t fallbackOffset() noexcept
        {
            return sizeof(TerrainWirePageDataHeader) +
                static_cast<std::size_t>(kHeightCount) *
                    sizeof(std::uint16_t) +
                static_cast<std::size_t>(kWeightBytes) * 2u +
                kHoleBytes +
                static_cast<std::size_t>(kTerrainWireMinMaxNodeCount) *
                    sizeof(std::uint16_t) * 2u;
        }
    } // namespace

    TerrainResources::TerrainResources(std::uint32_t capacity_pages)
        : capacity_pages_(std::max(capacity_pages, 1u))
        , fallback_capacity_pages_(std::max(capacity_pages_ * 4u, 1u))
    {
        free_slots_.reserve(capacity_pages_);
        for (std::uint32_t index = capacity_pages_; index != 0u; --index)
            free_slots_.push_back(index - 1u);
        free_fallback_slots_.reserve(fallback_capacity_pages_);
        for (std::uint32_t index = fallback_capacity_pages_;
             index != 0u;
             --index)
        {
            free_fallback_slots_.push_back(index - 1u);
        }
    }

    TerrainResources::~TerrainResources()
    {
        shutdownGpuCache();
    }

    bool TerrainResources::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        for (const auto& [_, page] : pages_)
        {
            if (!canRebaseRenderPageDelta(
                    page.header.origin.page_delta,
                    origin_delta))
            {
                return false;
            }
        }
        return true;
    }

    void TerrainResources::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        for (auto& [_, page] : pages_)
        {
            rebaseRenderPageDelta(
                page.header.origin.page_delta,
                origin_delta);
            uploadMetadata(page);
        }
    }

    std::string TerrainResources::key(TerrainWireId id)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(32u, '0');
        for (std::size_t index = 0u; index < 16u; ++index)
        {
            result[index * 2u] = digits[id.bytes[index] >> 4u];
            result[index * 2u + 1u] = digits[id.bytes[index] & 15u];
        }
        return result;
    }

    std::size_t TerrainResources::expectedPageBytes() noexcept
    {
        return sizeof(TerrainWirePageDataHeader) +
            static_cast<std::size_t>(kHeightCount) * sizeof(std::uint16_t) +
            static_cast<std::size_t>(kWeightBytes) * 2u +
            kHoleBytes +
            static_cast<std::size_t>(kTerrainWireMinMaxNodeCount) *
                sizeof(std::uint16_t) * 2u +
            static_cast<std::size_t>(kFallbackCount) * sizeof(std::uint16_t);
    }

    std::size_t TerrainResources::fallbackPageBytes() noexcept
    {
        return static_cast<std::size_t>(kFallbackCount) *
            sizeof(std::uint16_t);
    }

    std::size_t TerrainResources::fullPageStride() noexcept
    {
        return align16(expectedPageBytes());
    }

    std::size_t TerrainResources::fallbackPageStride() noexcept
    {
        return align16(fallbackPageBytes());
    }

    bool TerrainResources::initializeGpuCache(
        DeviceContext& device,
        DeferredDestroyQueue& deferred_destroy,
        std::uint32_t frames_in_flight)
    {
        if (device_ != nullptr)
            return device_ == &device;
        device_ = &device;
        deferred_destroy_ = &deferred_destroy;
        VmaAllocation full_allocation{nullptr};
        if (!createGpuBufferVmaBuffer(
                device.vmaAllocator(),
                static_cast<VkDeviceSize>(fullPageStride()) *
                    capacity_pages_,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                true,
                &full_page_buffer_,
                &full_allocation,
                reinterpret_cast<void**>(&full_page_mapped_)) ||
            full_page_buffer_ == VK_NULL_HANDLE || !full_page_mapped_)
        {
            full_page_allocation_ = full_allocation;
            shutdownGpuCache();
            return false;
        }
        full_page_allocation_ = full_allocation;

        VmaAllocation fallback_allocation{nullptr};
        if (!createGpuBufferVmaBuffer(
                device.vmaAllocator(),
                static_cast<VkDeviceSize>(fallbackPageStride()) *
                    fallback_capacity_pages_,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                true,
                &fallback_page_buffer_,
                &fallback_allocation,
                reinterpret_cast<void**>(&fallback_page_mapped_)) ||
            fallback_page_buffer_ == VK_NULL_HANDLE ||
            !fallback_page_mapped_)
        {
            fallback_page_allocation_ = fallback_allocation;
            shutdownGpuCache();
            return false;
        }
        fallback_page_allocation_ = fallback_allocation;

        const auto slot_count = std::max(frames_in_flight, 1u);
        page_metadata_cpu_.assign(fallback_capacity_pages_, GpuPageMeta{});
        page_metadata_slots_.resize(slot_count);
        for (auto& slot : page_metadata_slots_)
        {
            VmaAllocation metadata_allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    device.vmaAllocator(),
                    static_cast<VkDeviceSize>(sizeof(GpuPageMeta)) *
                        fallback_capacity_pages_,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    true,
                    &slot.buffer,
                    &metadata_allocation,
                    reinterpret_cast<void**>(&slot.mapped)) ||
                slot.buffer == VK_NULL_HANDLE || !slot.mapped)
            {
                slot.allocation = metadata_allocation;
                shutdownGpuCache();
                return false;
            }
            slot.allocation = metadata_allocation;
            std::memset(
                slot.mapped,
                0,
                sizeof(GpuPageMeta) * fallback_capacity_pages_);
            flushGpuBufferVmaAllocation(
                device.vmaAllocator(),
                metadata_allocation,
                0u,
                static_cast<VkDeviceSize>(sizeof(GpuPageMeta)) *
                    fallback_capacity_pages_);
        }
        selection_count_slots_.resize(slot_count);
        for (auto& slot : selection_count_slots_)
        {
            VmaAllocation count_allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    device.vmaAllocator(),
                    sizeof(std::uint32_t),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true,
                    &slot.buffer,
                    &count_allocation,
                    reinterpret_cast<void**>(&slot.mapped)) ||
                slot.buffer == VK_NULL_HANDLE || !slot.mapped)
            {
                slot.allocation = count_allocation;
                shutdownGpuCache();
                return false;
            }
            slot.allocation = count_allocation;
            *slot.mapped = 0u;
            flushGpuBufferVmaAllocation(
                device.vmaAllocator(),
                count_allocation,
                0u,
                sizeof(std::uint32_t));
        }
        std::memset(
            full_page_mapped_,
            0,
            fullPageStride() * capacity_pages_);
        std::memset(
            fallback_page_mapped_,
            0,
            fallbackPageStride() * fallback_capacity_pages_);
        flushGpuBufferVmaAllocation(
            device.vmaAllocator(),
            full_allocation,
            0u,
            static_cast<VkDeviceSize>(fullPageStride()) * capacity_pages_);
        flushGpuBufferVmaAllocation(
            device.vmaAllocator(),
            fallback_allocation,
            0u,
            static_cast<VkDeviceSize>(fallbackPageStride()) *
                fallback_capacity_pages_);
        for (auto& [_, page] : pages_)
        {
            uploadFallback(page);
            uploadFull(page);
            uploadMetadata(page);
        }
        return true;
    }

    void TerrainResources::shutdownGpuCache() noexcept
    {
        if (retire_scheduler_ && retire_owner_token_ != 0u)
        {
            retire_scheduler_->purge(retire_owner_token_);
            free_slots_.insert(
                free_slots_.end(),
                retiring_slots_.begin(),
                retiring_slots_.end());
            free_fallback_slots_.insert(
                free_fallback_slots_.end(),
                retiring_fallback_slots_.begin(),
                retiring_fallback_slots_.end());
        }
        retiring_slots_.clear();
        retiring_fallback_slots_.clear();
        if (device_)
        {
            const auto retire = [this](VkBuffer buffer, void* allocation)
            {
                if (buffer == VK_NULL_HANDLE)
                    return;
                if (deferred_destroy_)
                {
                    deferred_destroy_->retireBuffer(
                        buffer,
                        static_cast<VmaAllocation>(allocation));
                }
                else
                {
                    destroyGpuBufferVmaBuffer(
                        device_->vmaAllocator(),
                        buffer,
                        static_cast<VmaAllocation>(allocation));
                }
            };
            retire(full_page_buffer_, full_page_allocation_);
            retire(fallback_page_buffer_, fallback_page_allocation_);
            for (auto& slot : page_metadata_slots_)
                retire(slot.buffer, slot.allocation);
            for (auto& slot : selection_count_slots_)
            {
                retire(slot.buffer, slot.allocation);
                slot = {};
            }
        }
        full_page_buffer_ = VK_NULL_HANDLE;
        full_page_allocation_ = nullptr;
        full_page_mapped_ = nullptr;
        fallback_page_buffer_ = VK_NULL_HANDLE;
        fallback_page_allocation_ = nullptr;
        fallback_page_mapped_ = nullptr;
        page_metadata_slots_.clear();
        page_metadata_cpu_.clear();
        selection_count_slots_.clear();
        latest_selected_patch_count_ = 0u;
        selected_patch_count_valid_ = false;
        device_ = nullptr;
        deferred_destroy_ = nullptr;
        retire_scheduler_ = nullptr;
        retire_owner_token_ = 0u;
    }

    bool TerrainResources::accepts(
        TerrainWireId id,
        std::uint64_t revision) const noexcept
    {
        if (!id.valid() || revision == 0u)
            return false;
        const auto found = latest_revision_.find(key(id));
        return found == latest_revision_.end() || revision > found->second;
    }

    bool TerrainResources::promote(Page& page) noexcept
    {
        if (page.cache_slot != kInvalidSlot)
        {
            page.last_wanted_serial = frame_serial_;
            return true;
        }
        if (free_slots_.empty())
        {
            Page* oldest = nullptr;
            for (auto& [_, candidate] : pages_)
            {
                if (candidate.cache_slot != kInvalidSlot &&
                    (&candidate != &page) &&
                    (!oldest || candidate.last_wanted_serial <
                        oldest->last_wanted_serial))
                {
                    oldest = &candidate;
                }
            }
            if (!oldest)
                return false;
            demote(*oldest);
            // Slot reuse is fence-proven, not immediate. The old page may
            // still be sampled by an in-flight TerrainGBuffer draw.
            if (free_slots_.empty())
                return false;
        }
        page.cache_slot = free_slots_.back();
        free_slots_.pop_back();
        page.last_wanted_serial = frame_serial_;
        uploadFull(page);
        uploadMetadata(page);
        return true;
    }

    bool TerrainResources::ensureFallback(Page& page) noexcept
    {
        if (page.fallback_slot != kInvalidSlot)
            return true;
        if (free_fallback_slots_.empty())
        {
            Page* oldest = nullptr;
            for (auto& [_, candidate] : pages_)
            {
                if (&candidate == &page ||
                    candidate.fallback_slot == kInvalidSlot)
                {
                    continue;
                }
                if (!oldest ||
                    (candidate.cache_slot == kInvalidSlot &&
                        oldest->cache_slot != kInvalidSlot) ||
                    (candidate.cache_slot == oldest->cache_slot &&
                        candidate.last_wanted_serial <
                            oldest->last_wanted_serial))
                {
                    oldest = &candidate;
                }
            }
            if (!oldest)
                return false;
            releaseFallback(*oldest);
            if (free_fallback_slots_.empty())
                return false;
        }
        page.fallback_slot = free_fallback_slots_.back();
        free_fallback_slots_.pop_back();
        uploadFallback(page);
        uploadMetadata(page);
        return true;
    }

    void TerrainResources::uploadFull(Page& page) noexcept
    {
        if (!device_ || !full_page_mapped_ ||
            page.cache_slot == kInvalidSlot || page.data.empty())
        {
            return;
        }
        const auto offset = static_cast<std::size_t>(page.cache_slot) *
            fullPageStride();
        std::memcpy(
            full_page_mapped_ + offset,
            page.data.data(),
            page.data.size());
        flushGpuBufferVmaAllocation(
            device_->vmaAllocator(),
            static_cast<VmaAllocation>(full_page_allocation_),
            offset,
            fullPageStride());
    }

    void TerrainResources::uploadFallback(Page& page) noexcept
    {
        if (!device_ || !fallback_page_mapped_ ||
            page.fallback_slot == kInvalidSlot ||
            page.data.size() < fallbackOffset() + fallbackPageBytes())
        {
            return;
        }
        const auto offset = static_cast<std::size_t>(page.fallback_slot) *
            fallbackPageStride();
        std::memcpy(
            fallback_page_mapped_ + offset,
            page.data.data() + fallbackOffset(),
            fallbackPageBytes());
        flushGpuBufferVmaAllocation(
            device_->vmaAllocator(),
            static_cast<VmaAllocation>(fallback_page_allocation_),
            offset,
            fallbackPageStride());
    }

    void TerrainResources::uploadMetadata(Page& page) noexcept
    {
        if (!device_ || page_metadata_cpu_.empty() ||
            page.fallback_slot == kInvalidSlot)
        {
            return;
        }
        GpuPageMeta meta{};
        for (std::size_t axis = 0u; axis < 3u; ++axis)
        {
            meta.origin_full_slot[axis] =
                page.header.origin.page_delta[axis];
            meta.local_spacing[axis] = page.header.origin.local[axis];
        }
        meta.origin_full_slot[3] = page.cache_slot == kInvalidSlot
            ? -1
            : static_cast<std::int32_t>(page.cache_slot);
        meta.local_spacing[3] = page.header.sample_spacing;
        meta.height_extent[0] = page.header.height_min;
        meta.height_extent[1] = page.header.height_max;
        meta.height_extent[2] = page.header.sample_spacing *
            kTerrainWireQuadEdge;
        meta.height_extent[3] = 1.0f /
            (page.header.height_max - page.header.height_min);
        meta.fallback_flags[0] = page.fallback_slot;
        meta.fallback_flags[1] = 1u |
            (page.cache_slot == kInvalidSlot ? 0u : 2u) |
            ((page.transition_active ||
                page.transition_end_coverage > 0.0f) ? 4u : 0u);
        meta.fallback_flags[2] = page.header.weight_layer_count;
        meta.fallback_flags[3] = page.header.transition_seed;
        meta.transition[0] = page.transition_start_time;
        meta.transition[1] = page.transition_duration;
        meta.transition[2] = page.transition_start_coverage;
        meta.transition[3] = page.transition_end_coverage;
        meta.actual_height_bounds[0] = page.actual_height_min;
        meta.actual_height_bounds[1] = page.actual_height_max;
        page_metadata_cpu_[page.fallback_slot] = meta;
    }

    void TerrainResources::clearMetadata(std::uint32_t slot) noexcept
    {
        if (!device_ || page_metadata_cpu_.empty() ||
            slot >= fallback_capacity_pages_)
        {
            return;
        }
        page_metadata_cpu_[slot] = {};
    }

    std::uint32_t TerrainResources::pageMetadataBufferCount() const
        noexcept
    {
        return static_cast<std::uint32_t>(page_metadata_slots_.size());
    }

    VkBuffer TerrainResources::pageMetadataBuffer(
        std::uint32_t index) const noexcept
    {
        return index < page_metadata_slots_.size()
            ? page_metadata_slots_[index].buffer
            : VK_NULL_HANDLE;
    }

    std::uint32_t TerrainResources::selectionCountBufferCount() const
        noexcept
    {
        return static_cast<std::uint32_t>(selection_count_slots_.size());
    }

    VkBuffer TerrainResources::selectionCountBuffer(
        std::uint32_t index) const noexcept
    {
        return index < selection_count_slots_.size()
            ? selection_count_slots_[index].buffer
            : VK_NULL_HANDLE;
    }

    void TerrainResources::markSelectionSubmitted(
        std::uint32_t frame_index) noexcept
    {
        if (selection_count_slots_.empty())
            return;
        selection_count_slots_[
            frame_index % selection_count_slots_.size()].submitted = true;
    }

    void TerrainResources::onSelectionFrameBegin(
        std::uint32_t frame_index) noexcept
    {
        if (!device_)
            return;
        if (!page_metadata_slots_.empty() && !page_metadata_cpu_.empty())
        {
            auto& metadata = page_metadata_slots_[
                frame_index % page_metadata_slots_.size()];
            if (metadata.mapped)
            {
                const auto bytes = static_cast<VkDeviceSize>(
                    sizeof(GpuPageMeta) * page_metadata_cpu_.size());
                std::memcpy(
                    metadata.mapped,
                    page_metadata_cpu_.data(),
                    static_cast<std::size_t>(bytes));
                flushGpuBufferVmaAllocation(
                    device_->vmaAllocator(),
                    static_cast<VmaAllocation>(metadata.allocation),
                    0u,
                    bytes);
            }
        }
        if (selection_count_slots_.empty())
            return;
        auto& slot = selection_count_slots_[
            frame_index % selection_count_slots_.size()];
        if (!slot.submitted || !slot.mapped)
            return;
        invalidateGpuBufferVmaAllocation(
            device_->vmaAllocator(),
            static_cast<VmaAllocation>(slot.allocation),
            0u,
            sizeof(std::uint32_t));
        latest_selected_patch_count_ = *slot.mapped;
        selected_patch_count_valid_ = true;
        slot.submitted = false;
    }

    void TerrainResources::demote(Page& page) noexcept
    {
        if (page.cache_slot == kInvalidSlot)
            return;
        const auto slot = page.cache_slot;
        page.cache_slot = kInvalidSlot;
        uploadMetadata(page);
        deferFullSlotReturn(slot);
    }

    void TerrainResources::releaseFallback(Page& page) noexcept
    {
        if (page.fallback_slot == kInvalidSlot)
            return;
        const auto slot = page.fallback_slot;
        clearMetadata(slot);
        page.fallback_slot = kInvalidSlot;
        deferFallbackSlotReturn(slot);
    }

    void TerrainResources::deferFullSlotReturn(
        std::uint32_t slot) noexcept
    {
        if (!retire_scheduler_ || !deferred_destroy_ ||
            retire_owner_token_ == 0u)
        {
            free_slots_.push_back(slot);
            return;
        }
        retiring_slots_.push_back(slot);
        retire_scheduler_->defer(
            deferred_destroy_->currentSerial(),
            retire_owner_token_,
            [this, slot]
            {
                std::erase(retiring_slots_, slot);
                free_slots_.push_back(slot);
            });
    }

    void TerrainResources::deferFallbackSlotReturn(
        std::uint32_t slot) noexcept
    {
        if (!retire_scheduler_ || !deferred_destroy_ ||
            retire_owner_token_ == 0u)
        {
            free_fallback_slots_.push_back(slot);
            return;
        }
        retiring_fallback_slots_.push_back(slot);
        retire_scheduler_->defer(
            deferred_destroy_->currentSerial(),
            retire_owner_token_,
            [this, slot]
            {
                std::erase(retiring_fallback_slots_, slot);
                free_fallback_slots_.push_back(slot);
            });
    }

    bool TerrainResources::upsert(
        const UploadTerrainPagePayload& header,
        std::span<const std::byte> page_data)
    {
        if (header.scene_id.isNull() || !accepts(header.id, header.revision) ||
            !std::isfinite(header.height_min) ||
            !std::isfinite(header.height_max) ||
            !(header.height_max > header.height_min) ||
            !std::isfinite(header.sample_spacing) ||
            !(header.sample_spacing > 0.0f) ||
            !std::isfinite(header.geometric_error) ||
            header.geometric_error < 0.0f ||
            !std::isfinite(header.hlod_enter_error_pixels) ||
            !std::isfinite(header.hlod_exit_error_pixels) ||
            !(header.hlod_enter_error_pixels >
                header.hlod_exit_error_pixels) ||
            !(header.hlod_exit_error_pixels > 0.0f) ||
            header.transition_milliseconds == 0u ||
            header.transition_seed == 0u ||
            header.hierarchy_level > 4u ||
            header.child_count > 16u ||
            ((header.hierarchy_level == 0u) !=
                (header.child_count == 0u)) ||
            header.weight_layer_count > 8u ||
            page_data.size() != expectedPageBytes())
        {
            return false;
        }
        TerrainWirePageDataHeader data_header{};
        std::memcpy(&data_header, page_data.data(), sizeof(data_header));
        if (data_header.height_count != kHeightCount ||
            data_header.weight_plane_bytes != kWeightBytes ||
            data_header.hole_bytes != kHoleBytes ||
            data_header.min_max_node_count !=
                kTerrainWireMinMaxNodeCount ||
            data_header.fallback_height_count != kFallbackCount)
        {
            return false;
        }
        for (const auto value : header.origin.local)
            if (!std::isfinite(value))
                return false;
        for (std::uint8_t index = 0u; index < header.child_count; ++index)
        {
            if (!header.children[index].valid() ||
                header.children[index] == header.id)
            {
                return false;
            }
            for (std::uint8_t other = 0u; other < index; ++other)
                if (header.children[index] == header.children[other])
                    return false;
        }

        const auto page_key = key(header.id);
        auto found = pages_.find(page_key);
        std::uint64_t retained_serial = frame_serial_;
        std::optional<Page> previous;
        if (found != pages_.end())
        {
            retained_serial = found->second.last_wanted_serial;
            previous.emplace(std::move(found->second));
            pages_.erase(found);
        }
        Page page;
        page.header = header;
        page.header.page_data = {};
        page.data.assign(page_data.begin(), page_data.end());
        // The cooker writes the min/max pyramid from fine to coarse; its
        // final node is the page root. Use that canonical summary instead of
        // rescanning all 66,049 height samples on the render thread.
        const auto root_offset = minMaxOffset() +
            static_cast<std::size_t>(kTerrainWireMinMaxNodeCount - 1u) *
                sizeof(std::uint16_t) * 2u;
        std::uint16_t actual_quantized_min{0u};
        std::uint16_t actual_quantized_max{0u};
        std::memcpy(
            &actual_quantized_min,
            page.data.data() + root_offset,
            sizeof(actual_quantized_min));
        std::memcpy(
            &actual_quantized_max,
            page.data.data() + root_offset + sizeof(std::uint16_t),
            sizeof(actual_quantized_max));
        if (actual_quantized_min > actual_quantized_max)
        {
            if (previous)
                pages_.emplace(page_key, std::move(*previous));
            return false;
        }
        const auto quantization_extent =
            header.height_max - header.height_min;
        page.actual_height_min = header.height_min +
            quantization_extent *
                (static_cast<float>(actual_quantized_min) / 65535.0f);
        page.actual_height_max = header.height_min +
            quantization_extent *
                (static_cast<float>(actual_quantized_max) / 65535.0f);
        // A revision is immutable from the GPU's point of view. Do not
        // overwrite the previous revision's persistently mapped slots: an
        // older frame may still be sampling them. Allocate fresh slots, make
        // future frame metadata point at those slots, then fence-retire the
        // old revision after the replacement has been accepted.
        page.cache_slot = kInvalidSlot;
        page.fallback_slot = kInvalidSlot;
        page.last_wanted_serial = retained_serial;
        if (previous)
        {
            page.transition_start_time = previous->transition_start_time;
            page.transition_duration = previous->transition_duration;
            page.transition_start_coverage =
                previous->transition_start_coverage;
            page.transition_end_coverage =
                previous->transition_end_coverage;
            page.drawable_target = previous->drawable_target;
            page.transition_active = previous->transition_active;
        }
        else if (header.parent.valid())
        {
            page.transition_start_coverage = 0.0f;
            page.transition_end_coverage = 0.0f;
            page.drawable_target = false;
        }
        pages_.emplace(page_key, std::move(page));
        auto& stored = pages_.at(page_key);
        const auto rollback = [&]()
        {
            auto current = pages_.find(page_key);
            if (current != pages_.end())
            {
                if (!previous || current->second.fallback_slot !=
                        previous->fallback_slot)
                {
                    releaseFallback(current->second);
                }
                if (!previous || current->second.cache_slot !=
                        previous->cache_slot)
                {
                    demote(current->second);
                }
                pages_.erase(current);
            }
            if (previous)
            {
                // The old bytes and canonical metadata were deliberately
                // left untouched until commit, so rollback is a map restore
                // only. Re-uploading here would reintroduce a host-write vs
                // in-flight shader-read race.
                pages_.emplace(page_key, std::move(*previous));
            }
        };
        if (!ensureFallback(stored))
        {
            rollback();
            return false;
        }
        uploadFallback(stored);
        if (stored.cache_slot != kInvalidSlot)
            uploadFull(stored);
        uploadMetadata(stored);
        if (!promote(stored))
        {
            rollback();
            return false;
        }
        if (previous)
        {
            if (previous->fallback_slot != kInvalidSlot)
            {
                clearMetadata(previous->fallback_slot);
                deferFallbackSlotReturn(previous->fallback_slot);
            }
            if (previous->cache_slot != kInvalidSlot)
                deferFullSlotReturn(previous->cache_slot);
        }
        latest_revision_[page_key] = header.revision;
        return true;
    }

    bool TerrainResources::remove(
        TerrainWireId id,
        std::uint64_t revision) noexcept
    {
        if (!id.valid() || revision == 0u)
            return false;
        const auto page_key = key(id);
        auto& latest = latest_revision_[page_key];
        if (revision <= latest)
            return true;
        latest = revision;
        const auto found = pages_.find(page_key);
        if (found == pages_.end())
            return true;
        demote(found->second);
        releaseFallback(found->second);
        pages_.erase(found);
        return true;
    }

    const TerrainResources::Page* TerrainResources::find(
        TerrainWireId id) const noexcept
    {
        const auto found = pages_.find(key(id));
        return found == pages_.end() ? nullptr : &found->second;
    }

    bool TerrainResources::touch(TerrainWireId id) noexcept
    {
        const auto found = pages_.find(key(id));
        return found != pages_.end() && promote(found->second);
    }

    void TerrainResources::reconcileWanted(
        std::span<const ViewOrigin> views,
        float wanted_radius,
        float scene_time,
        std::uint64_t demotion_delay_frames) noexcept
    {
        if (!std::isfinite(scene_time))
            return;

        for (auto& [_, page] : pages_)
        {
            if (page.transition_active &&
                scene_time >= page.transition_start_time +
                    page.transition_duration)
            {
                page.transition_active = false;
                page.transition_start_coverage =
                    page.transition_end_coverage;
                page.transition_duration = 0.0f;
                uploadMetadata(page);
            }
        }

        std::unordered_map<std::string, bool> desired;
        desired.reserve(pages_.size());
        for (const auto& [page_key, _] : pages_)
            desired.emplace(page_key, false);

        const auto projectedError = [&views](const Page& page) noexcept
        {
            float maximum = 0.0f;
            const auto extent = static_cast<double>(
                page.header.sample_spacing) * kTerrainWireQuadEdge;
            for (const auto& view : views)
            {
                if (!std::isfinite(view.coordinate_page_size) ||
                    !(view.coordinate_page_size > 0.0f) ||
                    !std::isfinite(view.projection_scale) ||
                    !(view.projection_scale > 0.0f))
                {
                    continue;
                }
                const auto center_x = (
                        static_cast<double>(
                            page.header.origin.page_delta[0]) -
                        static_cast<double>(
                            view.position.page_delta[0])) *
                        view.coordinate_page_size +
                    page.header.origin.local[0] -
                    view.position.local[0] + extent * 0.5;
                const auto center_z = (
                        static_cast<double>(
                            page.header.origin.page_delta[2]) -
                        static_cast<double>(
                            view.position.page_delta[2])) *
                        view.coordinate_page_size +
                    page.header.origin.local[2] -
                    view.position.local[2] + extent * 0.5;
                const auto origin_y = (
                        static_cast<double>(
                            page.header.origin.page_delta[1]) -
                        static_cast<double>(
                            view.position.page_delta[1])) *
                        view.coordinate_page_size +
                    page.header.origin.local[1] -
                    view.position.local[1];
                // Screen error is governed by the nearest point of a page,
                // not by its centre.  The distinction is essential for sparse
                // fixed-grid HLOD: a finite World may occupy one corner of a
                // much larger canonical ancestor.  A camera directly above
                // that occupied child is inside the ancestor's XZ footprint,
                // even though it can be hundreds of kilometres from the
                // ancestor centre.  Centre distance therefore selected the
                // coarsest single-child ancestor and made the real terrain
                // occupy only a tiny sliver beneath correctly placed objects.
                const auto horizontal_half = extent * 0.5;
                const auto x = std::max(
                    std::abs(center_x) - horizontal_half, 0.0);
                const auto z = std::max(
                    std::abs(center_z) - horizontal_half, 0.0);
                const auto minimum_y = origin_y + page.actual_height_min;
                const auto maximum_y = origin_y + page.actual_height_max;
                const auto y = minimum_y > 0.0
                    ? minimum_y
                    : (maximum_y < 0.0 ? -maximum_y : 0.0);
                const auto distance = std::max(
                    std::sqrt(x * x + y * y + z * z), 1.0);
                maximum = std::max(
                    maximum,
                    static_cast<float>(page.header.geometric_error /
                        distance * view.projection_scale));
            }
            return maximum;
        };
        const auto select = [this, &desired, &projectedError](
            auto&& self,
            Page& page) -> void
        {
            // Stage asynchronous refinement one hierarchy level at a time.
            // Descending again while this page is only partly visible can
            // retire the sole coarse fallback before the newly arrived
            // grandchildren form a stable replacement.
            if (page.transition_active && page.drawable_target)
            {
                desired[key(page.header.id)] = true;
                return;
            }
            bool children_ready = page.header.child_count != 0u;
            std::array<Page*, 16u> children{};
            for (std::uint8_t index = 0u;
                 index < page.header.child_count;
                 ++index)
            {
                const auto found = pages_.find(key(
                    page.header.children[index]));
                if (found == pages_.end() ||
                    found->second.fallback_slot == kInvalidSlot)
                {
                    children_ready = false;
                    break;
                }
                children[index] = &found->second;
            }
            const auto threshold = page.drawable_target
                ? page.header.hlod_enter_error_pixels
                : page.header.hlod_exit_error_pixels;
            if (children_ready && projectedError(page) > threshold)
            {
                for (std::uint8_t index = 0u;
                     index < page.header.child_count;
                     ++index)
                {
                    self(self, *children[index]);
                }
                return;
            }
            desired[key(page.header.id)] = true;
        };
        for (auto& [_, page] : pages_)
        {
            if (!page.header.parent.valid() ||
                !pages_.contains(key(page.header.parent)))
            {
                select(select, page);
            }
        }
        for (auto& [page_key, page] : pages_)
            setDrawable(page, desired[page_key], scene_time);

        debug_view_surface_valid_ = false;
        debug_view_surface_level_ = 0u;
        debug_view_surface_height_ = 0.0f;
        debug_view_surface_clearance_ = 0.0f;
        if (!views.empty())
        {
            const auto& view = views.front();
            debug_view_position_ = view.position;
            float finest_spacing = std::numeric_limits<float>::infinity();
            for (const auto& [page_key, page] : pages_)
            {
                if (!desired[page_key] || page.data.size() <
                        sizeof(TerrainWirePageDataHeader) +
                            static_cast<std::size_t>(kHeightCount) *
                                sizeof(std::uint16_t) ||
                    !std::isfinite(view.coordinate_page_size) ||
                    !(view.coordinate_page_size > 0.0f))
                {
                    continue;
                }
                const double x = (
                        static_cast<double>(view.position.page_delta[0]) -
                        static_cast<double>(page.header.origin.page_delta[0])) *
                        view.coordinate_page_size +
                    view.position.local[0] - page.header.origin.local[0];
                const double z = (
                        static_cast<double>(view.position.page_delta[2]) -
                        static_cast<double>(page.header.origin.page_delta[2])) *
                        view.coordinate_page_size +
                    view.position.local[2] - page.header.origin.local[2];
                const double extent = static_cast<double>(
                    page.header.sample_spacing) * kTerrainWireQuadEdge;
                if (x < 0.0 || z < 0.0 || x > extent || z > extent ||
                    page.header.sample_spacing >= finest_spacing)
                {
                    continue;
                }

                const double sample_x = std::clamp(
                    x / page.header.sample_spacing,
                    0.0,
                    static_cast<double>(kTerrainWireQuadEdge));
                const double sample_z = std::clamp(
                    z / page.header.sample_spacing,
                    0.0,
                    static_cast<double>(kTerrainWireQuadEdge));
                const auto x0 = static_cast<std::uint32_t>(
                    std::floor(sample_x));
                const auto z0 = static_cast<std::uint32_t>(
                    std::floor(sample_z));
                const auto x1 = std::min(x0 + 1u, kTerrainWireQuadEdge);
                const auto z1 = std::min(z0 + 1u, kTerrainWireQuadEdge);
                const auto loadHeight = [&page](
                    std::uint32_t sx,
                    std::uint32_t sz) noexcept
                {
                    const auto index = static_cast<std::size_t>(sz) *
                        kTerrainWireSampleEdge + sx;
                    std::uint16_t quantized{0u};
                    std::memcpy(
                        &quantized,
                        page.data.data() +
                            sizeof(TerrainWirePageDataHeader) +
                            index * sizeof(quantized),
                        sizeof(quantized));
                    return page.header.height_min +
                        (page.header.height_max - page.header.height_min) *
                            (static_cast<float>(quantized) / 65535.0f);
                };
                const float tx = static_cast<float>(sample_x - x0);
                const float tz = static_cast<float>(sample_z - z0);
                const float h0 = std::lerp(
                    loadHeight(x0, z0), loadHeight(x1, z0), tx);
                const float h1 = std::lerp(
                    loadHeight(x0, z1), loadHeight(x1, z1), tx);
                const float local_height = std::lerp(h0, h1, tz);
                const double page_y =
                    static_cast<double>(page.header.origin.page_delta[1]) *
                        view.coordinate_page_size +
                    page.header.origin.local[1];
                const double view_y =
                    static_cast<double>(view.position.page_delta[1]) *
                        view.coordinate_page_size +
                    view.position.local[1];
                debug_view_surface_valid_ = true;
                debug_view_surface_level_ = page.header.hierarchy_level;
                debug_view_surface_height_ = static_cast<float>(
                    page_y + local_height);
                debug_view_surface_clearance_ = static_cast<float>(
                    view_y - page_y - local_height);
                finest_spacing = page.header.sample_spacing;
            }
        }

        struct Candidate final
        {
            Page* page{nullptr};
            double distance_squared{0.0};
            std::string_view stable_key;
            bool drawable{false};
        };
        std::vector<Candidate> candidates;
        candidates.reserve(pages_.size());
        const auto finite_radius = std::isfinite(wanted_radius) &&
            wanted_radius > 0.0f
            ? static_cast<double>(wanted_radius)
            : 0.0;
        for (auto& [page_key, page] : pages_)
        {
            double closest = std::numeric_limits<double>::infinity();
            const auto page_extent = static_cast<double>(
                page.header.sample_spacing) * kTerrainWireQuadEdge;
            for (const auto& view : views)
            {
                if (!std::isfinite(view.coordinate_page_size) ||
                    !(view.coordinate_page_size > 0.0f))
                {
                    continue;
                }
                const auto x = (
                        static_cast<double>(
                            page.header.origin.page_delta[0]) -
                        static_cast<double>(
                            view.position.page_delta[0])) *
                        view.coordinate_page_size +
                    static_cast<double>(page.header.origin.local[0]) -
                    view.position.local[0] + page_extent * 0.5;
                const auto z = (
                        static_cast<double>(
                            page.header.origin.page_delta[2]) -
                        static_cast<double>(
                            view.position.page_delta[2])) *
                        view.coordinate_page_size +
                    static_cast<double>(page.header.origin.local[2]) -
                    view.position.local[2] + page_extent * 0.5;
                closest = std::min(closest, x * x + z * z);
            }
            const auto reach = finite_radius + page_extent * 0.70710678118;
            if (closest <= reach * reach)
            {
                candidates.push_back({
                    &page,
                    closest,
                    page_key,
                    desired[page_key] || page.transition_active});
            }
        }
        std::ranges::sort(
            candidates,
            [](const Candidate& left, const Candidate& right)
            {
                if (left.drawable != right.drawable)
                    return left.drawable > right.drawable;
                if (left.distance_squared != right.distance_squared)
                    return left.distance_squared < right.distance_squared;
                return left.stable_key < right.stable_key;
            });
        const auto wanted_count = std::min<std::size_t>(
            candidates.size(), capacity_pages_);
        for (std::size_t index = 0u; index < wanted_count; ++index)
            (void)promote(*candidates[index].page);

        for (auto& [_, page] : pages_)
        {
            if (page.cache_slot != kInvalidSlot &&
                frame_serial_ > page.last_wanted_serial &&
                frame_serial_ - page.last_wanted_serial >
                    demotion_delay_frames)
            {
                demote(page);
            }
        }
    }

    float TerrainResources::coverageAt(
        const Page& page,
        float scene_time) const noexcept
    {
        if (!page.transition_active || page.transition_duration <= 0.0f)
            return page.transition_end_coverage;
        const auto progress = std::clamp(
            (scene_time - page.transition_start_time) /
                page.transition_duration,
            0.0f,
            1.0f);
        return std::lerp(
            page.transition_start_coverage,
            page.transition_end_coverage,
            progress);
    }

    void TerrainResources::setDrawable(
        Page& page,
        bool drawable,
        float scene_time) noexcept
    {
        if (page.drawable_target == drawable)
            return;
        const auto current = coverageAt(page, scene_time);
        page.drawable_target = drawable;
        page.transition_start_time = scene_time;
        page.transition_duration = std::max(
            static_cast<float>(page.header.transition_milliseconds) /
                1000.0f,
            0.001f);
        page.transition_start_coverage = current;
        page.transition_end_coverage = drawable ? 1.0f : 0.0f;
        page.transition_active = true;
        uploadMetadata(page);
    }

    TerrainPageCacheStatsReply TerrainResources::stats() const noexcept
    {
        TerrainPageCacheStatsReply result;
        result.resident_pages = static_cast<std::uint32_t>(pages_.size());
        result.capacity_pages = capacity_pages_;
        result.fallback_capacity_pages = fallback_capacity_pages_;
        result.selected_patch_count = latest_selected_patch_count_;
        result.selected_patch_count_valid =
            selected_patch_count_valid_ ? 1u : 0u;
        result.debug_view_surface_valid =
            debug_view_surface_valid_ ? 1u : 0u;
        result.debug_view_surface_level = debug_view_surface_level_;
        for (std::size_t axis = 0u; axis < 3u; ++axis)
        {
            result.debug_view_page_delta[axis] =
                debug_view_position_.page_delta[axis];
            result.debug_view_local[axis] = debug_view_position_.local[axis];
        }
        result.debug_view_surface_height = debug_view_surface_height_;
        result.debug_view_surface_clearance =
            debug_view_surface_clearance_;
        result.gpu_capacity_bytes =
            static_cast<std::uint64_t>(capacity_pages_) *
                fullPageStride() +
            static_cast<std::uint64_t>(fallback_capacity_pages_) *
                fallbackPageStride() +
            static_cast<std::uint64_t>(fallback_capacity_pages_) *
                sizeof(GpuPageMeta) * page_metadata_slots_.size();
        for (const auto& [_, page] : pages_)
        {
            if (page.header.hierarchy_level == 0u)
                ++result.fine_pages;
            else
                ++result.hlod_pages;
            if (page.transition_active)
                ++result.transition_pages;
            if (page.transition_active ||
                page.transition_end_coverage > 0.0f)
            {
                ++result.drawable_pages;
                if (page.header.hierarchy_level <
                        std::size(result.drawable_pages_by_level))
                {
                    ++result.drawable_pages_by_level[
                        page.header.hierarchy_level];
                }
            }
            result.cpu_resident_bytes += page.data.size();
            if (page.fallback_slot != kInvalidSlot)
                result.gpu_resident_bytes +=
                    fallbackPageStride() +
                    sizeof(GpuPageMeta) * page_metadata_slots_.size();
            else
                ++result.gpu_unavailable_pages;
            if (page.cache_slot == kInvalidSlot &&
                page.fallback_slot != kInvalidSlot)
                ++result.fallback_pages;
            if (page.cache_slot != kInvalidSlot)
            {
                ++result.full_resolution_pages;
                result.gpu_resident_bytes += fullPageStride();
            }
        }
        return result;
    }
} // namespace lux::render
