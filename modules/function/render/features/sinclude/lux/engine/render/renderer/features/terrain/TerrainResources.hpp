#pragma once
#include <lux/engine/function/render/features/visibility.h>

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/features/terrain/TerrainOperation.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/FrameRetireScheduler.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class DeviceContext;
    class DeferredDestroyQueue;

    class LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC TerrainResources final
    {
    public:
        struct alignas(16) GpuPageMeta final
        {
            std::int32_t origin_full_slot[4]{};
            float local_spacing[4]{};
            float height_extent[4]{};
            std::uint32_t fallback_flags[4]{};
            float transition[4]{};
            float actual_height_bounds[4]{};
        };

        static_assert(sizeof(GpuPageMeta) == 96u);

        struct alignas(16) GpuTerrainPatch final
        {
            std::uint32_t page_lod_xy[4]{};
            std::uint32_t slots_flags[4]{};
        };

        static_assert(sizeof(GpuTerrainPatch) == 32u);

        struct ViewOrigin final
        {
            RenderLargePosition3D position;
            float coordinate_page_size{1024.0f};
            float projection_scale{1.0f};
        };

        struct Page final
        {
            UploadTerrainPagePayload header;
            std::vector<std::byte> data;
            std::uint32_t cache_slot{0xffffffffu};
            std::uint32_t fallback_slot{0xffffffffu};
            std::uint64_t last_wanted_serial{0u};
            float transition_start_time{0.0f};
            float transition_duration{0.0f};
            float transition_start_coverage{1.0f};
            float transition_end_coverage{1.0f};
            float actual_height_min{0.0f};
            float actual_height_max{0.0f};
            bool drawable_target{true};
            bool transition_active{false};
        };

        struct CreateInfo
        {
            DeviceContext& device;
            DeferredDestroyQueue& retirement;
            FrameRetireScheduler& callbacks;
            std::uint32_t capacity_pages;
            std::uint32_t frames_in_flight;
        };

        using CreateResult = Expected<std::unique_ptr<TerrainResources>>;
        [[nodiscard]] static CreateResult create(const CreateInfo& info) noexcept;
        ~TerrainResources() noexcept;

        TerrainResources(const TerrainResources&) = delete;
        TerrainResources& operator=(const TerrainResources&) = delete;
        TerrainResources(TerrainResources&&) = delete;
        TerrainResources& operator=(TerrainResources&&) = delete;

        [[nodiscard]] bool accepts(TerrainWireId id, std::uint64_t revision) const noexcept;
        [[nodiscard]] bool upsert(const UploadTerrainPagePayload& header, std::span<const std::byte> page_data);
        [[nodiscard]] bool remove(TerrainWireId id, std::uint64_t revision) noexcept;
        [[nodiscard]] const Page* find(TerrainWireId id) const noexcept;

        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept;
        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept;

        /// Marks a page wanted by one View and promotes it to a full-resolution
        /// slot. If the fixed cache is full, the least-recently-wanted page is
        /// demoted first; its embedded parent fallback remains resident.
        [[nodiscard]] bool touch(TerrainWireId id) noexcept;
        void reconcileWanted(
            std::span<const ViewOrigin> views,
            float wanted_radius,
            float scene_time,
            std::uint64_t demotion_delay_frames = 120u
        ) noexcept;

        void beginFrame() noexcept
        {
            ++frame_serial_;
        }

        [[nodiscard]] TerrainPageCacheStatsReply stats() const noexcept;

        [[nodiscard]] std::uint32_t capacityPages() const noexcept
        {
            return capacity_pages_;
        }

        [[nodiscard]] static std::size_t expectedPageBytes() noexcept;
        [[nodiscard]] static std::size_t fallbackPageBytes() noexcept;
        [[nodiscard]] static std::size_t fullPageStride() noexcept;
        [[nodiscard]] static std::size_t fallbackPageStride() noexcept;
        [[nodiscard]] VkBuffer fullPageBuffer() const noexcept;
        [[nodiscard]] VkBuffer fallbackPageBuffer() const noexcept;

        [[nodiscard]] VkBuffer pageMetadataBuffer() const noexcept
        {
            return pageMetadataBuffer(0u);
        }

        [[nodiscard]] std::uint32_t pageMetadataBufferCount() const noexcept;
        [[nodiscard]] VkBuffer pageMetadataBuffer(std::uint32_t index) const noexcept;

        [[nodiscard]] std::uint32_t fallbackCapacityPages() const noexcept
        {
            return fallback_capacity_pages_;
        }

        [[nodiscard]] std::uint32_t selectionCountBufferCount() const noexcept;
        [[nodiscard]] VkBuffer selectionCountBuffer(std::uint32_t index) const noexcept;
        void markSelectionSubmitted(std::uint32_t frame_index) noexcept;
        void onSelectionFrameBegin(std::uint32_t frame_index) noexcept;

    private:
        struct Backing;
        TerrainResources(const CreateInfo& info, std::unique_ptr<Backing> backing) noexcept;
        [[nodiscard]] FrameRetireScheduler::OwnerToken ownerToken() const noexcept;

        [[nodiscard]] static std::string key(TerrainWireId id);
        [[nodiscard]] bool promote(Page& page) noexcept;
        [[nodiscard]] bool ensureFallback(Page& page) noexcept;
        void uploadFull(Page& page) noexcept;
        void uploadFallback(Page& page) noexcept;
        void uploadMetadata(Page& page) noexcept;
        void clearMetadata(std::uint32_t slot) noexcept;
        void demote(Page& page) noexcept;
        void releaseFallback(Page& page) noexcept;
        void deferFullSlotReturn(std::uint32_t slot) noexcept;
        void deferFallbackSlotReturn(std::uint32_t slot) noexcept;
        [[nodiscard]] float coverageAt(const Page& page, float scene_time) const noexcept;
        void setDrawable(Page& page, bool drawable, float scene_time) noexcept;

        std::unordered_map<std::string, Page> pages_;
        std::unordered_map<std::string, std::uint64_t> latest_revision_;
        std::vector<std::uint32_t> free_slots_;
        std::vector<std::uint32_t> free_fallback_slots_;
        std::uint32_t capacity_pages_{0u};
        std::uint32_t fallback_capacity_pages_{0u};
        std::uint64_t frame_serial_{1u};
        VmaAllocator allocator_;
        FrameRetireScheduler& retire_scheduler_;
        std::unique_ptr<Backing> backing_;
        std::vector<GpuPageMeta> page_metadata_cpu_;
        std::uint32_t latest_selected_patch_count_{0u};
        bool selected_patch_count_valid_{false};
        bool debug_view_surface_valid_{false};
        std::uint8_t debug_view_surface_level_{0u};
        RenderLargePosition3D debug_view_position_{};
        float debug_view_surface_height_{0.0f};
        float debug_view_surface_clearance_{0.0f};
    };
} // namespace lux::render
