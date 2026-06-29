#pragma once
/**
 * @file IVertexSource.hpp
 * @brief Vertex-source abstraction — the renderer's first-class concept for
 *        "where do this draw's vertices come from".
 *
 * Stage R1.1 of the render-refactor (.internal/render_refactor_plan.md).
 *
 * Background:
 *
 *   Before this refactor, the renderer hard-coded the assumption that vertex
 *   data lived in a single global VBO read via vertex attributes. That made
 *   any compute-generated vertex pipeline (skinning, morph targets, GPU
 *   foliage, virtual geometry) an awkward special case. IVertexSource lifts
 *   that assumption: every drawable references a vertex source, and the
 *   source decides how the data gets to the GPU.
 *
 * Implementations land in subsequent R1.x commits:
 *
 *   - StaticVertexSource    (R1.2) — backed by the existing global VBO,
 *                                     re-exposed as an SSBO view; zero-copy.
 *   - TransientVertexSource (R1.3) — per-frame compute-written pool;
 *                                     producers (skinning, morph, cloth)
 *                                     allocate ranges each frame.
 *   - VirtualGeometrySource (future) — placeholder for Nanite-style streamed
 *                                       geometry.
 *
 * Wire-up to the rest of the renderer (R1.4 + R1.5):
 *
 *   - VertexPoolRegistry exposes registered sources as a bindless SSBO
 *     array under EDescriptorSetSlot::VertexPool (new slot, added in R1.4).
 *   - InstanceProperty (R1.5) gains vertex_pool_id / vertex_base /
 *     vertex_count fields so vertex shaders can do, uniformly:
 *
 *       Vertex v = vertex_pools[p.vertex_pool_id].data[p.vertex_base + gl_VertexIndex];
 *
 *     with no per-kind branching.
 */

#include <cstdint>

#include <vulkan/vulkan.h>

#include <lux/engine/render/core/VertexLayoutTypes.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /// Categorical hint about how the source produces its data.
    /// Used by the registry to decide lifecycle / barrier semantics, NOT by
    /// shader code (which is uniform across kinds — that's the whole point).
    enum class EVertexSourceKind : std::uint8_t
    {
        StaticPool      = 0,  ///< Resident; written once at upload time.
        TransientPool   = 1,  ///< Per-frame; compute writes, render reads.
        VirtualGeometry = 2,  ///< (Reserved) Nanite-style streamed geometry.
    };

    /// Reference to a contiguous range of vertices in a registered pool.
    /// Layout matches the InstanceProperty fields (R1.5) verbatim so CPU
    /// and GPU views agree without manual packing.
    struct VertexSourceHandle
    {
        /// Index into VertexPoolRegistry's bindless array. ~0u = invalid.
        std::uint32_t pool_id      = ~0u;
        /// First vertex (in vertices, not bytes) within the pool's buffer.
        std::uint32_t vertex_base  = 0;
        /// Number of vertices the handle refers to.
        std::uint32_t vertex_count = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return pool_id != ~0u;
        }

        [[nodiscard]] friend constexpr bool
        operator==(const VertexSourceHandle& a, const VertexSourceHandle& b) noexcept
        {
            return a.pool_id == b.pool_id
                && a.vertex_base == b.vertex_base
                && a.vertex_count == b.vertex_count;
        }
    };
    static_assert(sizeof(VertexSourceHandle) == 12,
                  "VertexSourceHandle must remain trivially packable for GPU upload.");

    inline constexpr VertexSourceHandle kInvalidVertexSourceHandle{};

    /// Vertex source abstract base.
    ///
    /// Lifetime: owned by either RenderContext (for StaticVertexSource —
    /// one instance, lifetime = engine session) or by a feature
    /// (TransientVertexSource — owned by SkinningFeature etc., lifetime =
    /// feature). The VertexPoolRegistry (R1.4) holds non-owning pointers
    /// so it can rebuild its descriptor writes when pools come and go.
    class LUX_FUNCTION_PUBLIC IVertexSource
    {
    public:
        virtual ~IVertexSource();   // anchor in IVertexSource.cpp

        IVertexSource()                                = default;
        IVertexSource(const IVertexSource&)            = delete;
        IVertexSource& operator=(const IVertexSource&) = delete;
        IVertexSource(IVertexSource&&)                 = delete;
        IVertexSource& operator=(IVertexSource&&)      = delete;

        /// What kind of source this is. Used by the registry / RG to decide
        /// barrier domains. Not consumed by mesh shaders.
        [[nodiscard]] virtual EVertexSourceKind kind() const noexcept = 0;

        /// The backing GPU buffer. Bound as an SSBO into the
        /// VertexPoolRegistry bindless array.
        [[nodiscard]] virtual VkBuffer buffer() const noexcept = 0;

        /// Vertex layout the buffer is laid out in. For a heterogeneous-layout
        /// future we'd promote layout to the handle level; today the
        /// invariant is "one source = one layout".
        [[nodiscard]] virtual VertexLayoutId layout() const noexcept = 0;

        /// Stable index into the bindless vertex-pool array. Assigned by
        /// VertexPoolRegistry::registerSource; the source itself stores it
        /// and returns it here so handles can be filled in O(1).
        [[nodiscard]] virtual std::uint32_t bindlessPoolId() const noexcept = 0;

        /// VertexPoolRegistry calls this on registerSource() to assign a
        /// pool id. Implementations should store it for bindlessPoolId().
        /// Not meant to be called by client code; only the registry has
        /// authority over pool-id assignment.
        virtual void setBindlessPoolId(std::uint32_t id) noexcept = 0;

        // ---- allocation ----
        //
        // Static sources: allocate is called at mesh upload time; free at
        // mesh asset unload. Handles are stable for the lifetime of the
        // mesh asset.
        //
        // Transient sources: allocate is called every frame by a producer;
        // free is a no-op (the entire arena resets in beginFrame). Handles
        // are valid for one frame only.
        //
        // Virtual geometry (future): allocate maps to LOD selection +
        // page residency; semantics TBD.

        [[nodiscard]] virtual VertexSourceHandle
        allocate(std::uint32_t vertex_count) = 0;

        virtual void free(VertexSourceHandle handle) = 0;

        // ---- diagnostics ----

        [[nodiscard]] virtual std::uint32_t totalCapacity() const noexcept = 0;
        [[nodiscard]] virtual std::uint32_t usedCapacity()  const noexcept = 0;
    };

} // namespace lux::render
