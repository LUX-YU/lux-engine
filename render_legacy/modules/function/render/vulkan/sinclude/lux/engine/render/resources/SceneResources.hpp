#pragma once
/**
 * @file SceneResources.hpp
 * @brief GPU resources for the scene descriptor set (Set 0).
 *
 * Manages two SoA SSBO buffers bound at Set 0:
 *   binding 0 — SceneGlobalGpuData[]  (one slot per RenderScene)
 *   binding 1 — ViewGpuData[]         (one slot per View)
 *
 * Each RenderScene holds a SlotHandle from allocateScene().
 * Each View holds a SlotHandle from allocateView().
 * Shaders access data via push constants scene_index / view_index.
 */

#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>     // Viewport
#include <lux/engine/function/render/client/core/RenderViewTypes.hpp> // CameraView, EntityTransform
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/lifecycle/DescriptorRevision.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <Eigen/Dense>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    // =========================================================================
    //  SceneGlobalGpuData
    // =========================================================================

    struct alignas(16) SceneGlobalGpuData
    {
        float time_sec;         // 4B
        float delta_time;       // 4B
        uint32_t frame_number;  // 4B
        float maintenance_time; // 4B → 16B; transitions/retirement keep advancing while simulation pauses
    };
    static_assert(sizeof(SceneGlobalGpuData) == 16);

    // =========================================================================
    //  ViewGpuData + fillViewGpuData()
    // =========================================================================

    struct alignas(16) ViewGpuData
    {
        float view[16];                  // 64B  col-major mat4
        float proj[16];                  // 64B
        float inv_view[16];              // 64B
        float inv_proj[16];              // 64B
        float cam_pos[4];                // 16B  xyz + w=1
        float viewport[4];               // 16B  width, height, near_z, far_z
        std::int32_t camera_page[4];     // xyz relative to RenderScene origin page
        float camera_local_page_size[4]; // local xyz + coordinate page size
    };
    static_assert(sizeof(ViewGpuData) == 320);
    static_assert(sizeof(ViewGpuData) % 16 == 0);
    static_assert(
        sizeof(ViewGpuData) == kViewDataStrideBytes,
        "ViewGpuData must match the neutral per-view stride (RenderTypes.hpp)"
    );

    /// Recover the camera's positive near/far distances from the engine's
    /// Vulkan-ZO projection matrix.  ViewGpuData::viewport.zw is consumed as
    /// near/far by LinearDepth, Fog, Water and SSAO; filling it from
    /// VkViewport::minDepth/maxDepth (normally 0/1) collapses every background
    /// sample to zero metres and leaves the environment black.
    [[nodiscard]] inline bool projectionDepthRange(
        const Eigen::Matrix4f& projection,
        float& near_z,
        float& far_z
    ) noexcept
    {
        constexpr float kEpsilon = 1.0e-5f;
        const float a = projection(2, 2);
        const float b = projection(2, 3);
        const float c = projection(3, 2);

        float recovered_near = 0.0f;
        float recovered_far = 0.0f;
        if (std::abs(c) > kEpsilon)
        {
            if (std::abs(a) < kEpsilon || std::abs(a - c) < kEpsilon)
            {
                return false;
            }
            recovered_near = std::abs(-b / a);
            recovered_far = std::abs(-b / (a - c));
        }
        else
        {
            // Engine Vulkan-ZO orthographic form:
            // a=-1/(far-near), b=-near/(far-near), w=1.
            if (std::abs(a) < kEpsilon || std::abs(projection(3, 3) - 1.0f) > kEpsilon)
            {
                return false;
            }
            recovered_near = std::abs(b / a);
            recovered_far = recovered_near + std::abs(1.0f / a);
        }
        if (!std::isfinite(recovered_near) || !std::isfinite(recovered_far))
        {
            return false;
        }
        if (recovered_far < recovered_near)
        {
            std::swap(recovered_near, recovered_far);
        }
        if (recovered_near <= 0.0f || recovered_far - recovered_near < kEpsilon)
        {
            return false;
        }
        near_z = recovered_near;
        far_z = recovered_far;
        return true;
    }

    inline void fillViewGpuData(
        const CameraView& cv,
        const EntityTransform& ct,
        const Viewport& vp,
        const RenderLargePosition3D& render_origin,
        float coordinate_page_size,
        ViewGpuData& out
    )
    {
        auto copy = [](const Eigen::Matrix4f& M, float* dst) { std::memcpy(dst, M.data(), sizeof(float) * 16); };
        copy(cv.view, out.view);
        copy(cv.proj, out.proj);
        copy(cv.inv_view, out.inv_view);
        copy(cv.inv_proj, out.inv_proj);

        out.cam_pos[0] = ct.position.x();
        out.cam_pos[1] = ct.position.y();
        out.cam_pos[2] = ct.position.z();
        out.cam_pos[3] = 1.f;

        out.viewport[0] = vp.width;
        out.viewport[1] = vp.height;
        float near_z = 0.0f;
        float far_z = 0.0f;
        if (!projectionDepthRange(cv.proj, near_z, far_z))
        {
            // A malformed/custom projection must remain finite for GPU
            // consumers.  The ordinary 2D/3D engine projections always take
            // the recovered path above.
            near_z = 0.1f;
            far_z = 1000.0f;
        }
        out.viewport[2] = near_z;
        out.viewport[3] = far_z;

        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            out.camera_page[axis] = render_origin.page_delta[axis];
            out.camera_local_page_size[axis] = render_origin.local[axis];
        }
        out.camera_page[3] = 0;
        out.camera_local_page_size[3] = coordinate_page_size;
    }

    // =========================================================================
    //  SceneResources
    // =========================================================================

    class LUX_FUNCTION_PUBLIC SceneResources final
    {
    public:
        static constexpr EUploadPhase kUploadPhase = EUploadPhase::POST_UPLOAD;

        using SceneGlobalBuffer = DynamicSSBO<SceneGlobalGpuData>;
        using ViewBuffer = DynamicSSBO<ViewGpuData>;

        struct CreateInfo
        {
            DeviceContext& device_context;
            DeferredDestroyQueue& deferred_queue;
            uint32_t slices;                      ///< frames_in_flight
            uint32_t initial_scene_capacity;      ///< initial SceneGlobal SoA capacity (e.g. 4–8)
            uint32_t initial_view_capacity;       ///< initial View SoA capacity (e.g. 4–8)
            SceneDescriptorArena* arena{nullptr}; ///< per-scene set allocator (growable)
            VkDescriptorSetLayout set_layout;     ///< Set 0 layout (2 SSBO bindings), shared/global
            uint32_t binding_scene_global = static_cast<uint32_t>(ESceneSetBindings::GLOBAL);
            uint32_t binding_view_data = static_cast<uint32_t>(ESceneSetBindings::VIEW);

            /// Complete per-frame domain targets. The scene arena owns these sets.
            std::span<const VkDescriptorSet> domain_sets{};
            uint32_t domain_binding_offset{0};
        };

        using CreateResult = Expected<std::unique_ptr<SceneResources>>;
        [[nodiscard]] static CreateResult create(const CreateInfo& info) noexcept;

        SceneResources(const SceneResources&) = delete;
        SceneResources& operator=(const SceneResources&) = delete;
        SceneResources(SceneResources&&) = delete;
        SceneResources& operator=(SceneResources&&) = delete;

        // ── Scene slot lifecycle (one slot per RenderScene) ───────────────────

        [[nodiscard]] Expected<SlotHandle> allocateScene()
        {
            const uint32_t old_gen = scene_buf_.bufferGeneration();
            auto h = scene_buf_.allocate();
            if (scene_buf_.bufferGeneration() != old_gen)
            {
                ds_revision_.bump();
            }
            return h;
        }

        void freeScene(const SlotHandle& slot)
        {
            scene_buf_.free(slot);
        }

        // ── View slot lifecycle (one slot per View) ───────────────────────────

        [[nodiscard]] Expected<SlotHandle> allocateView()
        {
            const uint32_t old_gen = view_buf_.bufferGeneration();
            auto h = view_buf_.allocate();
            if (view_buf_.bufferGeneration() != old_gen)
            {
                ds_revision_.bump();
            }
            return h;
        }

        void freeView(const SlotHandle& slot)
        {
            view_buf_.free(slot);
        }

        // ── Batch pre-allocation ─────────────────────────────────────────────

        /// Pre-reserve scene-global slots to avoid mid-frame reallocation.
        [[nodiscard]] bool reserveScenes(uint32_t count)
        {
            const uint32_t old_gen = scene_buf_.bufferGeneration();
            if (!scene_buf_.reserve(count))
            {
                return false;
            }
            if (scene_buf_.bufferGeneration() != old_gen)
            {
                ds_revision_.bump();
            }
            return true;
        }

        /// Pre-reserve view slots to avoid mid-frame reallocation.
        [[nodiscard]] bool reserveViews(uint32_t count)
        {
            const uint32_t old_gen = view_buf_.bufferGeneration();
            if (!view_buf_.reserve(count))
            {
                return false;
            }
            if (view_buf_.bufferGeneration() != old_gen)
            {
                ds_revision_.bump();
            }
            return true;
        }

        // ── Per-frame API ────────────────────────────────────────────────────

        void beginFrame(uint32_t slice)
        {
            current_slice_ = slice;

            if (ds_revision_.needsWrite(current_slice_))
            {
                writeDescriptorForSet(current_slice_);
            }
        }

        void writeSceneGlobal(const SlotHandle& slot, const SceneGlobalGpuData& data)
        {
            scene_buf_.write(current_slice_, slot, data);
        }

        void writeView(const SlotHandle& slot, const ViewGpuData& data)
        {
            view_buf_.write(current_slice_, slot, data);
        }

        /// Write view data using an explicit slice index instead of current_slice_.
        /// Use this when the caller knows the target frame slice independently of
        /// when beginFrame() was last called (e.g. updateView() before beginFrame()).
        void writeView(const SlotHandle& slot, const ViewGpuData& data, uint32_t slice)
        {
            view_buf_.write(slice, slot, data);
        }

        /// Write RAW per-view GPU-data bytes into the view SoA slot — domain-neutral:
        /// SceneResources does not interpret the bytes (a feature lays out the camera
        /// struct). size must equal the per-view stride. Used by beginFrame to upload
        /// View::view_data_staging after the slot fence wait (#27).
        void writeViewData(const SlotHandle& slot, const void* bytes, std::size_t size, uint32_t slice)
        {
            assert(size == sizeof(ViewGpuData) && "view data stride mismatch");
            ViewGpuData tmp;
            std::memcpy(&tmp, bytes, sizeof(tmp));
            view_buf_.write(slice, slot, tmp);
        }

        /// Flush dirty ranges and emit pipeline barriers for both buffers.
        void endFrame(VkCommandBuffer cb)
        {
            scene_buf_.flush(current_slice_);
            view_buf_.flush(current_slice_);

            auto bar0 = scene_buf_.getBarrier(current_slice_);
            auto bar1 = view_buf_.getBarrier(current_slice_);

            VkBufferMemoryBarrier2 barriers[2];
            int b_count = 0;
            if (bar0)
            {
                barriers[b_count++] = *bar0;
            }
            if (bar1)
            {
                barriers[b_count++] = *bar1;
            }

            if (b_count > 0)
            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = static_cast<uint32_t>(b_count);
                dep.pBufferMemoryBarriers = barriers;
                vkCmdPipelineBarrier2(cb, &dep);
            }

            scene_buf_.resetDirtySlice(current_slice_);
            view_buf_.resetDirtySlice(current_slice_);
        }

        void onFrameBeginMaintenance(const FrameStamp& stamp)
        {
            beginFrame(stamp.slotIndex());
        }

        // ── TransferScheduler integration ──────────────────────────────────────────

        /// Flush dirty ranges and submit host-write barriers to the scheduler.
        void submitTransfers(TransferScheduler& scheduler)
        {
            scene_buf_.flush(current_slice_);
            view_buf_.flush(current_slice_);

            if (auto bar0 = scene_buf_.getBarrier(current_slice_))
            {
                scheduler.submitExtraPostBarrier(*bar0);
            }
            if (auto bar1 = view_buf_.getBarrier(current_slice_))
            {
                scheduler.submitExtraPostBarrier(*bar1);
            }

            scene_buf_.resetDirtySlice(current_slice_);
            view_buf_.resetDirtySlice(current_slice_);
        }

        // ── Descriptor set access ─────────────────────────────────────────────

        [[nodiscard]] VkDescriptorSet getDescriptorSet() const
        {
            return descriptor_sets_[current_slice_];
        }

        [[nodiscard]] VkDescriptorSet getDescriptorSet(uint32_t slice) const
        {
            return (slice < descriptor_sets_.size()) ? descriptor_sets_[slice] : VK_NULL_HANDLE;
        }

    private:
        struct Backing;
        SceneResources(const CreateInfo& info, Backing&& backing) noexcept;
        void writeDescriptorForSet(uint32_t set_index) noexcept;

        /// Domain targets and canonical targets are both used by current render consumers.
        DomainWriteTarget domain_{};

        DescriptorRevision ds_revision_{};
        uint32_t current_slice_{0};
        uint32_t binding_scene_global_{0};
        uint32_t binding_view_data_{1};

        SceneGlobalBuffer scene_buf_;
        ViewBuffer view_buf_;

        std::vector<VkDescriptorSet> descriptor_sets_; // size = FIF, both bindings bound
    };

} // namespace lux::render
