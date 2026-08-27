#pragma once
/**
 * @file SkinningResources.hpp
 * @brief Per-scene GPU resources for compute vertex skinning.
 *
 * Owns the two GPU buffers the skinning compute pass needs that aren't
 * already provided elsewhere:
 *
 *   - bone_palette_buffer_  : CPU-writable SSBO of mat4 (one entry per bone
 *                              per skinned instance, packed back-to-back).
 *                              Filled each frame from the bone-palette upload
 *                              command.
 *   - output_pool_          : a TransientVertexSource holding the
 *                              skinned vertex output. Registered with the
 *                              VertexPoolRegistry so it gets a bindless
 *                              pool id that the graphics _vp shaders read.
 *
 * The *input* vertices come from MeshResources' global VBO, exposed as a
 * StaticVertexSource in the bindless pool. Ownership of that registration
 * lives in the per-scene StaticVertexPoolSet; SkinningResources no longer holds
 * an input source — the dispatch caller passes the input pool id (fetched
 * from StaticVertexPoolSet) as part of queueDispatch().
 *
 * Lives in the scene registry (RenderScene::sceneRegistry), mirroring
 * ParticleResources. SkinningFeature finds/creates it, owns the
 * compute pipeline + descriptor set, and drives the per-frame dispatch
 * list built here.
 *
 * Frame model: beginFrame() resets the per-frame palette write cursor and
 * the dispatch list; the transient output pool resets too. Producers
 * (the upload-command handler + RenderableSystem skeletal path) call
 * uploadBonePalette() + queueDispatch() during command processing;
 * SkinningFeature consumes dispatches() in its RG compute pass.
 */

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <lux/engine/render/resources/vertex/TransientVertexSource.hpp>
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // kMaxFramesInFlight
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/function/render/client/core/RenderErrorList.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class DeviceContext;
    class VertexPoolRegistry;

    /// std430 mat4 — one bone's world-space skinning matrix.
    struct alignas(16) BoneMatrixGpu
    {
        float m[16];
    };
    static_assert(sizeof(BoneMatrixGpu) == 64);

    /// Workgroup local size of the skinning compute kernel — must match
    /// `layout(local_size_x = ...)` in `skin_compute.comp`.
    inline constexpr std::uint32_t kSkinWorkgroupSize = 64u;

    /// std430 — one entry per pending skinning dispatch this frame. Read by
    /// `skin_compute.comp` via the dispatch-params SSBO. `workgroup_start` is
    /// the running prefix sum of `ceil(vertex_count / kSkinWorkgroupSize)` so
    /// the shader can binary-search by `gl_WorkGroupID.x` to recover its
    /// dispatch index in a single batched `vkCmdDispatch`. Replaces
    /// the per-instance `vkCmdPushConstants` + `vkCmdDispatch` loop.
    struct SkinDispatchParams
    {
        std::uint32_t workgroup_start; ///< running prefix sum (key for binary search)
        std::uint32_t vertex_count;
        std::uint32_t in_base;
        std::uint32_t out_base;
        std::uint32_t palette_base;
        std::uint32_t in_pool_id;
    };
    static_assert(sizeof(SkinDispatchParams) == 24);

    class LUX_FUNCTION_PUBLIC SkinningResources
    {
    public:
        struct InitInfo
        {
            DeviceContext* device_context = nullptr;
            VertexPoolRegistry* vertex_pool_registry = nullptr;
            VertexLayoutId layout_id = kInvalidVertexLayoutId;
            std::uint32_t vertex_stride = 0;                      ///< sizeof(rdesc::Vertex)
            std::uint32_t max_bones = 64u * 1024;                 ///< palette capacity (entries)
            std::uint32_t max_dispatches = 4096u;                 ///< dispatch SSBO capacity (entries)
            VkDeviceSize output_pool_bytes = 16ull * 1024 * 1024; ///< skinned vtx pool
        };

        /// One skinning dispatch = one skinned mesh instance for this frame.
        struct Dispatch
        {
            std::uint32_t in_pool_id; ///< input bindless pool id (global VBO's registered id)
            std::uint32_t in_base;    ///< first input vertex
            std::uint32_t out_base;   ///< first output vertex (transient pool)
            std::uint32_t vertex_count;
            std::uint32_t palette_base; ///< first bone in bone_palette_buffer_
            std::uint32_t bone_count;
        };

        SkinningResources();
        ~SkinningResources();

        SkinningResources(const SkinningResources&) = delete;
        SkinningResources& operator=(const SkinningResources&) = delete;

        bool init(const InitInfo& info);
        void shutdown();

        /// Reset per-frame state: palette write cursor, dispatch list, and
        /// the transient output pool arena.
        void beginFrame() noexcept;

        /// Lazy per-frame reset keyed by the monotonic frame serial. The
        /// UploadBonePalette command handler calls this on every bone upload;
        /// the first call of a new frame resets, subsequent calls are no-ops.
        /// Used because client commands are drained BEFORE the render-thread
        /// frame-begin, so an onBeginFrame reset would wipe the just-appended
        /// dispatches.
        /// 自发上报的去处(非拥有)。dispatch 参数缓冲装不下本帧的批,是每帧的
        /// 资源仲裁结果 —— 没有调用方可以处置,但也不能不可见。
        void setErrorSink(RenderErrorSink* sink) noexcept
        {
            error_sink_ = sink;
        }

        void beginFrameIfNew(std::uint64_t frame_serial) noexcept
        {
            if (frame_serial != last_frame_serial_)
            {
                last_frame_serial_ = frame_serial;
                current_fi_ = static_cast<std::uint32_t>(frame_serial % kMaxFramesInFlight);
                beginFrame();
            }
        }

        /// Append a bone palette (one mat4 per bone). Returns the palette
        /// base index, or ~0u if the palette buffer is full this frame.
        [[nodiscard]] std::uint32_t uploadBonePalette(const BoneMatrixGpu* bones, std::uint32_t bone_count);

        /// Reserve a skinned-vertex output range + record a dispatch.
        /// Returns the output VertexSourceHandle (pool id + base + count) so
        /// the caller can point the instance's InstanceProperty at it.
        /// Returns an invalid handle if the output pool is exhausted.
        [[nodiscard]] VertexSourceHandle queueDispatch(
            std::uint32_t in_pool_id,
            std::uint32_t in_base,
            std::uint32_t vertex_count,
            std::uint32_t palette_base,
            std::uint32_t bone_count
        );

        [[nodiscard]] const std::vector<Dispatch>& dispatches() const noexcept
        {
            return dispatches_;
        }

        /// Dispatch-params SSBO for ring slot @p fi (per frame-in-flight).
        /// Filled by `uploadDispatches()` from the queued `dispatches_` list.
        [[nodiscard]] VkBuffer dispatchParamsBuffer(std::uint32_t fi) const noexcept
        {
            return dispatch_params_buffers_[fi % kMaxFramesInFlight];
        }

        /// Convert this frame's queued `dispatches_` into GPU-side
        /// `SkinDispatchParams[]` (with running `workgroup_start` prefix sum)
        /// in the current ring slot's SSBO. Returns the total number of
        /// workgroups for the batched `vkCmdDispatch(total, 1, 1)`, or 0 if
        /// nothing to dispatch / capacity exceeded.
        [[nodiscard]] std::uint32_t uploadDispatches() noexcept;

        /// Per-frame dispatch capacity (max entries fit in the ring slot).
        [[nodiscard]] std::uint32_t maxDispatches() const noexcept
        {
            return max_dispatches_;
        }

        /// Bone palette buffer for ring slot @p fi (per frame-in-flight).
        [[nodiscard]] VkBuffer bonePaletteBuffer(std::uint32_t fi) const noexcept
        {
            return bone_palette_buffers_[fi % kMaxFramesInFlight];
        }
        /// Current ring slot (== serial % kMaxFramesInFlight); set by beginFrameIfNew.
        /// Single source of truth shared by the palette write and the skin
        /// descriptor-set resolver so they always reference the same slot.
        [[nodiscard]] std::uint32_t currentFrameIndex() const noexcept
        {
            return current_fi_;
        }
        [[nodiscard]] TransientVertexSource& outputPool() noexcept
        {
            return output_pool_;
        }
        [[nodiscard]] const TransientVertexSource& outputPool() const noexcept
        {
            return output_pool_;
        }
        [[nodiscard]] std::uint32_t outputPoolId() const noexcept
        {
            return output_pool_.bindlessPoolId();
        }

        [[nodiscard]] bool initialized() const noexcept
        {
            return initialized_;
        }

    private:
        DeviceContext* device_ctx_{nullptr};
        VertexPoolRegistry* vertex_pool_registry_{nullptr};

        // Input vertex pool: ownership lives in the per-scene
        // StaticVertexPoolSet. SkinningResources no longer touches it; the
        // skinning dispatch carries the input pool id as a push constant.

        // Bone palette is CPU-written each frame and GPU-read by the skinning
        // compute pass, so it is ring-buffered per frame-in-flight: the CPU
        // may write frame N+1's palette while the GPU still reads frame N's. We
        // always allocate kMaxFramesInFlight slots and index by
        // serial % kMaxFramesInFlight — that never collides for any FIF count
        // <= kMaxFramesInFlight (reuse of slot S%k at frame S+k waits on frame
        // S's fence), so no runtime FIF count is needed.
        std::array<VkBuffer, kMaxFramesInFlight> bone_palette_buffers_{};
        std::array<VmaAllocation, kMaxFramesInFlight> bone_palette_allocs_{};
        std::array<void*, kMaxFramesInFlight> bone_palette_mapped_{};
        std::array<std::uint32_t, kMaxFramesInFlight> palette_cursors_{}; ///< next free bone slot, per fi
        std::uint32_t max_bones_{0};
        RenderErrorSink* error_sink_{nullptr};
        std::uint32_t current_fi_{0}; ///< serial % kMaxFramesInFlight (set in beginFrameIfNew)

        // Per-FIF dispatch-params SSBO ring. uploadDispatches() writes
        // SkinDispatchParams[dispatch_count] into the current slot each frame;
        // the skinning compute reads it via descriptor binding 2 and
        // binary-searches by gl_WorkGroupID.x to recover its dispatch entry.
        std::array<VkBuffer, kMaxFramesInFlight> dispatch_params_buffers_{};
        std::array<VmaAllocation, kMaxFramesInFlight> dispatch_params_allocs_{};
        std::array<void*, kMaxFramesInFlight> dispatch_params_mapped_{};
        std::uint32_t max_dispatches_{0};

        // Skinned-vertex OUTPUT pool is single-buffered. NOTE: this is a KNOWN
        // cross-frame WAR hazard, not a safe design — frame N+1's skinning compute
        // overwrites the arena frame N's vertex shader may still be reading, and
        // submission order does NOT order the two across frames (review P1#9). The
        // prior "submissions execute in order so it's safe" claim was wrong; the
        // bone palette above is per-FIF precisely to avoid this. Impact is a subtle
        // one-frame-ahead skin under GPU overlap, not corruption; the fix (ring the
        // output pool per frame-in-flight) is deferred. See TransientVertexSource.hpp.
        TransientVertexSource output_pool_;
        bool output_registered_{false};

        std::vector<Dispatch> dispatches_;
        std::uint64_t last_frame_serial_{~0ull}; ///< beginFrameIfNew key
        bool initialized_{false};
    };

} // namespace lux::render
