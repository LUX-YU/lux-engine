#pragma once
/**
 * @file TransientVertexSource.hpp
 * @brief Per-frame, compute-written vertex pool.
 *
 * Stage R1.3 of the render-refactor.
 *
 * Use case: producers (SkinningFeature, MorphTargetFeature, ClothFeature,
 * future GPU foliage / impostor features) call allocate(N) at the start
 * of their frame to reserve a vertex range, then a compute pass writes
 * into that range. Downstream mesh-rendering features read it via the
 * bindless VertexPool array — the entry index appears in InstanceProperty.
 *
 * Lifetime model:
 *
 *   - One VkBuffer, allocated once (typically 16 MiB), held for the
 *     lifetime of its owning scene resource.
 *   - Arena offset resets to 0 on beginFrame(). All handles allocated
 *     in the previous frame become invalid; producers must re-allocate
 *     every frame.
 *
 * Producer/consumer ordering inside a frame is enforced by RGBuilder's
 * declared read/write deps (existing infrastructure).
 *
 * KNOWN LIMITATION — cross-frame WAR (review P1#9): this pool is SINGLE-
 * buffered and the arena resets to offset 0 every beginFrame(), so frame
 * N+1's producer compute writes the SAME memory frame N's vertex shader
 * read. The RGBuilder dep only orders producer→consumer WITHIN one frame;
 * it does NOT order frame N+1's compute against frame N's still-in-flight
 * vertex read. With frames-in-flight (>=2) those overlap on the GPU, so a
 * heavily-overlapped frame can skin one frame ahead of what is drawn (a
 * subtle temporal artifact, not corruption). The earlier "submission order
 * guarantees safety" claim here was WRONG — Vulkan gives no such cross-
 * submission ordering without a barrier/semaphore. The correct fix is to
 * ring-buffer the pool per frame-in-flight (mirroring SkinningResources'
 * bone palette), which also requires the bindless VertexPool entry to
 * resolve to the current frame's slot. Deferred — see the per-FIF
 * skinned-output-pool follow-up task.
 *
 * Capacity overflow: allocate() returns kInvalidVertexSourceHandle when
 * the arena is exhausted. The caller decides how to degrade (skip this
 * frame's skinning, drop morph targets, etc.). A capacity-status
 * heuristic can trigger a buffer growth on the next frame boundary,
 * but for R1.3 we ship with fixed capacity and report overflow.
 */

#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/resources/vertex/IVertexSource.hpp>

#include <cstdint>
#include <memory>

namespace lux::render
{
    class DeviceContext;

    class LUX_FUNCTION_PUBLIC TransientVertexSource final : public IVertexSource
    {
    public:
        struct CreateInfo
        {
            DeviceContext* device_context = nullptr;
            VkDeviceSize capacity_bytes = 16ull * 1024 * 1024; ///< 16 MiB default
            VertexLayoutId layout_id = kInvalidVertexLayoutId;
            std::uint32_t vertex_stride = 0; ///< must be > 0
        };

        using CreateResult = Expected<std::unique_ptr<TransientVertexSource>>;
        [[nodiscard]] static CreateResult create(const CreateInfo& info) noexcept;
        ~TransientVertexSource() override = default;

        TransientVertexSource(const TransientVertexSource&) = delete;
        TransientVertexSource& operator=(const TransientVertexSource&) = delete;

        TransientVertexSource(TransientVertexSource&&) = delete;
        TransientVertexSource& operator=(TransientVertexSource&&) = delete;

        /// Reset arena offset. Producers must call this before their per-frame
        /// allocate() calls. Conventionally hooked into FrameServices'
        /// onBeginFrame so it runs once per frame in a known order.
        void beginFrame() noexcept;

        // ---- IVertexSource ----

        [[nodiscard]] EVertexSourceKind kind() const noexcept override
        {
            return EVertexSourceKind::TRANSIENT_POOL;
        }

        [[nodiscard]] VkBuffer buffer() const noexcept override
        {
            return buffer_.buffer();
        }

        [[nodiscard]] VertexLayoutId layout() const noexcept override
        {
            return layout_id_;
        }

        [[nodiscard]] std::uint32_t bindlessPoolId() const noexcept override
        {
            return pool_id_;
        }

        void setBindlessPoolId(std::uint32_t id) noexcept override
        {
            pool_id_ = id;
        }

        // ---- 本类特有(不再是 IVertexSource 的一部分) ----

        /// Reserve a vertex range. Returns kInvalidVertexSourceHandle on
        /// arena exhaustion. Thread-safe under the current engine model
        /// (single producer thread).
        [[nodiscard]] VertexSourceHandle allocate(std::uint32_t vertex_count);

    private:
        TransientVertexSource(VmaBuffer buffer, VertexLayoutId layout, std::uint32_t total_vertices) noexcept;

        VmaBuffer buffer_;
        VertexLayoutId layout_id_;
        std::uint32_t pool_id_{~0u};

        std::uint32_t next_vertex_{0};
        std::uint32_t total_vertices_{0};
    };

} // namespace lux::render
