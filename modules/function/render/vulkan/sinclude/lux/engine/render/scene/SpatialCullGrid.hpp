#pragma once
/**
 * @file SpatialCullGrid.hpp
 * @brief Cell-level spatial cull grid (spatial-cell-grid coarse cull).
 *
 * Slices space into uniform 2D XY cells (a spatial hash: worldPos -> cellId —
 * **not a tree**; each cell spans full Z height), and activates/dormants
 * cells by their distance to the cull source (the main camera). Instances in
 * a dormant cell are kept out of the per-frame GPU cull dispatch via a
 * per-slot active mask — this is what actually keeps down the "number of
 * instances active at any one time," which is the real scalability lever for
 * large worlds (see the design doc, §1.5). Large-world streaming is the
 * typical customer, but any scene that wants coarse distance/cell culling can
 * use it too (it's a mechanism, not a business concept).
 *
 * This is a ResourceRegistry component (like InstanceResources), driven
 * by SpatialCullFeature's onFrameBegin call to update() (after the main
 * camera is ready, before feature cull). It does **not** replace the existing
 * per-instance frustum+HZB cull — that remains the per-frame visibility path
 * (the traditional UE5-style approach, kept as-is). The spatial grid's only
 * job is to drop dormant-cell instances from the candidate set by distance.
 * Frustum / occlusion culling is still handled by per-instance cull + the
 * double-buffered HZB.
 *
 * See .internal/world-partition-design-2026-06-17.md (§3 design / §5 phased
 * rollout / §6 integration points / §7 architecture-fit: explicitly rules out
 * a per-instance octree, two-level GPU cull, and frustum-level cell culling).
 */

#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    class DeviceContext;
    class DeferredDestroyQueue;
    class InstanceResources;

    // =========================================================================
    //  SpatialCullGrid
    // =========================================================================
    class LUX_FUNCTION_PUBLIC SpatialCullGrid
    {
    public:
        struct InitInfo
        {
            DeviceContext*        device_context{nullptr};
            DeferredDestroyQueue* deferred_queue{nullptr};
            uint32_t              frames_in_flight{2};      ///< Determines the mask ring's slot count (FIF+1)
            uint32_t              initial_capacity{4096};   ///< Pre-allocated per-slot mask element count
            float                 cell_size{128.0f};        ///< Cell edge length (world units; engine default ~128m)
            float                 cull_distance{512.0f};    ///< Cull distance (a cell beyond this from the camera -> dormant)
        };

        SpatialCullGrid() = default;
        ~SpatialCullGrid();

        SpatialCullGrid(const SpatialCullGrid&)            = delete;
        SpatialCullGrid& operator=(const SpatialCullGrid&) = delete;

        void init(const InitInfo& info);
        void shutdown();

        /// World-space XY of one alive instance (cell assignment only uses
        /// the bsphere center's XY; cells span full Z). The low-level update
        /// overload consumes this directly, so SpatialCullGrid isn't tightly
        /// coupled to the concrete InstanceResources type (and headless GPU
        /// tests don't need to construct a full InstanceResources).
        struct InstanceXY
        {
            uint32_t slot;
            float    x;
            float    y;
        };

        /// Per frame: assigns instances to cells by world bsphere center,
        /// activates cells by distance, fills the per-slot active mask, and
        /// uploads it to the current ring slot. @p serial de-duplicates:
        /// multiple calls within the same frame (same serial) compute only
        /// once and share one buffer (the Forward / Deferred / Shadow cull
        /// passes all import the same mask).
        /// @p cameras = world-space camera positions (XYZ) of the active
        /// views; empty means everything stays active (no culling, matching
        /// the current behavior).
        /// Naturally friendly to dynamic instances: cell membership uses a
        /// hash, so a moving instance is an O(1) rehash (rebuilt every frame).
        void update(uint64_t                                  serial,
                    std::span<const std::array<float, 3>>     cameras,
                    const InstanceResources&                  instances);

        /// Low-level update: fed directly with alive instances' (slot, world
        /// xy). The convenience overload extracts bsphere data from
        /// InstanceResources and delegates here; serial de-duplication, ring
        /// advance, and mask upload all happen in this function.
        void update(uint64_t                                  serial,
                    std::span<const std::array<float, 3>>     cameras,
                    std::span<const InstanceXY>               alive,
                    uint32_t                                  slot_count);

        /// Current frame's per-slot active mask buffer (uint per slot;
        /// active=1 / dormant=0). Kept for tests / compatibility. After
        /// init(), always returns a valid buffer (pre-filled all-active).
        [[nodiscard]] VkBuffer activeMaskBuffer() const noexcept;

        /// GPU device address (buffer-device-address) of the current frame's
        /// active mask buffer. The cull dispatch passes it to
        /// mesh_cull_unified.comp via push-constant, and the shader reads it
        /// through buffer_reference — replacing a fixed descriptor binding 8
        /// so that coarse culling is opt-in: when SpatialCullGrid doesn't
        /// exist, the caller passes 0 and the shader skips the mask read
        /// (= everything active). Returns 0 (sentinel) if not init'd or the
        /// current slot is empty.
        [[nodiscard]] uint64_t activeMaskAddress() const noexcept;

        /// Number of slots covered by the mask (= instances.slotCount() from the last update()).
        [[nodiscard]] uint32_t maskSlotCount() const noexcept { return mask_slot_count_; }

        /// Reads back the current ring slot's host-mapped mask (active=1 /
        /// dormant=0), for tests to verify GPU upload contents only.
        /// host-coherent mapping, so what's read matches exactly what this
        /// frame's cull dispatch sees. Returns null if not init'd.
        [[nodiscard]] const uint32_t* gpuMaskMappedForTest() const noexcept;

        // ── Runtime-tunable ──
        void setCellSize(float s) noexcept;
        void setCullDistance(float r) noexcept;
        void setEnabled(bool e) noexcept { enabled_ = e; }   ///< off -> everything active (baseline for measuring coarse-cull gains)

        [[nodiscard]] bool  enabled() const noexcept       { return enabled_; }
        [[nodiscard]] float cellSize() const noexcept      { return cell_size_; }
        [[nodiscard]] float cullDistance() const noexcept  { return cull_distance_; }

        // ── Pure-function core (no GPU / no state dependency; unit-testable on CPU) ──
        /// World XY coordinates -> integer cell coordinates (the hash part of the spatial hash, not a tree).
        static void cellCoord(float world_x, float world_y, float cell_size,
                              int32_t& out_cx, int32_t& out_cy) noexcept;
        /// Whether cell (cx,cy) is within cull range of any camera. Range =
        /// cull_distance + one cell of margin (conservative: uses 2D distance
        /// to the cell center, preferring to over-activate rather than
        /// mis-cull). Empty cameras -> true (no culling).
        static bool cellInRange(int32_t cx, int32_t cy, float cell_size, float cull_distance,
                                std::span<const std::array<float, 3>> cameras) noexcept;

        // ── Debug stats (results from the last update(); used to verify coarse culling: active instance count drops as the camera moves away) ──
        [[nodiscard]] uint32_t activeInstanceCount() const noexcept { return stat_active_instances_; }
        [[nodiscard]] uint32_t totalInstanceCount()  const noexcept { return stat_total_instances_; }
        [[nodiscard]] uint32_t activeCellCount()     const noexcept { return stat_active_cells_; }
        [[nodiscard]] uint32_t totalCellCount()      const noexcept { return stat_total_cells_; }

    private:
        // Cell coordinate key (2D XY integer grid).
        struct CellKey
        {
            int32_t x{0};
            int32_t y{0};
            bool operator==(const CellKey&) const noexcept = default;
        };
        struct CellKeyHash
        {
            std::size_t operator()(const CellKey& k) const noexcept
            {
                return std::hash<std::uint64_t>{}(
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) << 32)
                    | static_cast<std::uint32_t>(k.y));
            }
        };

        void recomputeMask(std::span<const std::array<float, 3>> cameras,
                           std::span<const InstanceXY>           alive,
                           uint32_t                              slot_count);
        void uploadMask();         ///< Grows the current ring slot's buffer if needed, then memcpy's mask_
        void destroyRing() noexcept;

        DeviceContext*        device_ctx_{nullptr};
        DeferredDestroyQueue* deferred_queue_{nullptr};

        float cell_size_{128.0f};
        float cull_distance_{512.0f};
        bool  enabled_{true};
        bool  initialized_{false};

        // ── CPU mask + cell-active cache (rebuilt every frame) ──
        std::vector<uint32_t>                              mask_;            ///< per-slot active flag
        std::unordered_map<CellKey, uint8_t, CellKeyHash>  cell_active_;     ///< Lazy cell -> active cache
        std::vector<InstanceXY>                            extract_scratch_; ///< Reused scratch buffer for the convenience update() overload's alive-instance extraction
        uint32_t mask_slot_count_{0};

        // ── per-FIF active-mask GPU ring ──
        // One independent VkBuffer per FIF (RG's importBuffer is a whole-
        // buffer model, offset 0), CPU-mapped host-coherent. A reused ring
        // slot was last written at least (FIF+1) frames ago -> the GPU is
        // idle with it, so writing it can't tear an in-flight frame. See the
        // InstanceResources mdc_info ring for the same pattern.
        std::vector<VkBuffer>      ring_buffers_;
        std::vector<VmaAllocation> ring_allocs_;
        std::vector<void*>         ring_mapped_;
        std::vector<VkDeviceSize>  ring_sizes_;
        uint32_t ring_cursor_{0};
        uint32_t current_slot_{0};
        uint64_t last_upload_serial_{~0ull};

        // ── Debug stats ──
        uint32_t stat_active_instances_{0};
        uint32_t stat_total_instances_{0};
        uint32_t stat_active_cells_{0};
        uint32_t stat_total_cells_{0};
    };

} // namespace lux::render
