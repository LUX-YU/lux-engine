#pragma once
/**
 * @file StaticVertexSource.hpp
 * @brief IVertexSource impl backed by MeshResources's global VBO.
 *
 * Stage R1.2 of the render-refactor.
 *
 * Design summary:
 *
 *   - One StaticVertexSource per (VertexLayoutId, vbo_segment) pair. The
 *     pool exposes a single VkBuffer (the segment's VBO) as an SSBO via
 *     the VertexPoolRegistry's bindless array (R1.4).
 *
 *   - Mesh upload remains the responsibility of MeshResources::allocateOnly()
 *     plus the async upload worker — this class does NOT allocate ranges.
 *     allocate() returns
 *     kInvalidVertexSourceHandle by design; callers should use
 *     handleForMesh(MeshHandle) to construct a handle for an already-
 *     uploaded mesh. This keeps the existing arena / staging pipeline
 *     intact and the abstraction additive.
 *
 *   - Multi-segment fan-out: a single VertexLayoutId may use multiple
 *     ChainedArena segments as the scene grows. For R1, only segment 0
 *     is wrapped; multi-segment support (one StaticVertexSource per
 *     segment, fanning out the bindless array) is deferred to a later
 *     R-stage. With a 64 MiB default segment and ~50 B/vertex, segment 0
 *     fits ~1.3 M vertices — plenty for current scene scales.
 *
 *   - Buffer usage flags: R1.4 adds VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
 *     to MeshResources VBO creation so the bindless array can sample it.
 *     R1.2 alone doesn't bind anything, so the bit isn't required yet.
 */

#include <cstdint>

#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/vertex/IVertexSource.hpp>

namespace lux::render
{
    class MeshResources;

    class LUX_FUNCTION_PUBLIC StaticVertexSource final : public IVertexSource
    {
    public:
        /// @param meshes        Owning MeshResources (non-owning ptr; must
        ///                       outlive the source — typically RenderContext
        ///                       owns both with matching lifetime).
        /// @param vbo_segment   Which ChainedArena segment of MeshResources
        ///                       this source wraps. Today only 0 is wired.
        /// @param layout_id     Vertex layout in this pool.
        /// @param bindless_pool_id  Index assigned by VertexPoolRegistry
        ///                            on registration. Stored so handleForMesh
        ///                            can fill it into VertexSourceHandle.
        StaticVertexSource(MeshResources& meshes, std::uint16_t vbo_segment, VertexLayoutId layout_id) noexcept;

        ~StaticVertexSource() override;

        // ---- IVertexSource ----

        [[nodiscard]] EVertexSourceKind kind() const noexcept override
        {
            return EVertexSourceKind::StaticPool;
        }

        [[nodiscard]] VkBuffer buffer() const noexcept override;
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

        // (曾有 allocate / free 两个 override:前者无条件返回无效句柄、后者是空函数
        //  体——两个都不兑现基类声明的语义。随基类上那两个纯虚一并删除,见
        //  IVertexSource.hpp。两个零调用点的 capacity 诊断实现同删。
        //  静态网格的分配一直归 MeshResources::allocateOnly() + 上传 worker,
        //  取句柄走下面的 handleForMesh —— 那才是这个类真正的入口。)

        // ---- Static-source-specific API ----

        /// Translate a MeshHandle into a VertexSourceHandle referring to that
        /// mesh's vertex range within this pool. Returns kInvalidVertexSourceHandle
        /// if @p mesh is dead or its segment doesn't match this source's segment.
        ///
        /// The vertex_base / vertex_count fields are computed as:
        ///   vertex_base  = mesh.vertex_buffer_range.offset / vertex_stride
        ///   vertex_count = mesh.vertex_buffer_range.size   / vertex_stride
        [[nodiscard]] VertexSourceHandle handleForMesh(MeshHandle mesh) const noexcept;

    private:
        MeshResources* meshes_;
        std::uint16_t vbo_segment_;
        VertexLayoutId layout_id_;
        std::uint32_t pool_id_;
    };

} // namespace lux::render
