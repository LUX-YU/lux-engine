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

#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /// Categorical hint about how the source produces its data.
    /// Used by the registry to decide lifecycle / barrier semantics, NOT by
    /// shader code (which is uniform across kinds — that's the whole point).
    enum class EVertexSourceKind : std::uint8_t
    {
        StaticPool = 0,      ///< Resident; written once at upload time.
        TransientPool = 1,   ///< Per-frame; compute writes, render reads.
        VirtualGeometry = 2, ///< (Reserved) Nanite-style streamed geometry.
    };

    /// Reference to a contiguous range of vertices in a registered pool.
    /// Layout matches the InstanceProperty fields (R1.5) verbatim so CPU
    /// and GPU views agree without manual packing.
    struct VertexSourceHandle
    {
        /// Index into VertexPoolRegistry's bindless array. ~0u = invalid.
        std::uint32_t pool_id = ~0u;
        /// First vertex (in vertices, not bytes) within the pool's buffer.
        std::uint32_t vertex_base = 0;
        /// Number of vertices the handle refers to.
        std::uint32_t vertex_count = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return pool_id != ~0u;
        }

        [[nodiscard]] friend constexpr bool
        operator==(const VertexSourceHandle& a, const VertexSourceHandle& b) noexcept
        {
            return a.pool_id == b.pool_id && a.vertex_base == b.vertex_base && a.vertex_count == b.vertex_count;
        }
    };
    static_assert(
        sizeof(VertexSourceHandle) == 12,
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
        virtual ~IVertexSource(); // anchor in IVertexSource.cpp

        IVertexSource() = default;
        IVertexSource(const IVertexSource&) = delete;
        IVertexSource& operator=(const IVertexSource&) = delete;
        IVertexSource(IVertexSource&&) = delete;
        IVertexSource& operator=(IVertexSource&&) = delete;

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

        // ── 这里曾经有 allocate / free 两个纯虚，以及 totalCapacity /
        //    usedCapacity 两个诊断纯虚。全部删掉，理由分两类：
        //
        //  · allocate / free —— **没有任何实现兑现过这个契约**。原注释按三种实现
        //    分别写了分配语义(静态源在上传期分配、瞬态源每帧分配、虚拟几何映射到
        //    LOD 选择)，而实际上 `StaticVertexSource::allocate` 无条件返回无效句柄、
        //    两个 `free` 都是空函数体。照着注释调用的人拿到的是**静默的无效句柄**。
        //    而两个真实的分配路径都不经过这个接口:
        //      - 瞬态: `SkinningResources` 直接对它持有的 `TransientVertexSource`
        //        **值成员**调 allocate(具体类型，不是多态)
        //      - 静态: 分配归 `MeshResources::allocateOnly()` + 上传 worker，
        //        取句柄走 `StaticVertexSource::handleForMesh()`——它本就不在基类上
        //    ⇒ 经 `IVertexSource&` 调 allocate/free 的地方，全仓为 0。
        //
        //    (没有拆成 IAllocatingVertexSource / IMeshBackedVertexSource 两个能力
        //     接口:那会造出两个各只有一个实现、零多态调用点的新抽象。真正的问题是
        //     基类多承诺了，不是基类不够细。)
        //
        //  · totalCapacity / usedCapacity —— 诚实但零调用点的诊断面。
        //
        // 留下的 5 个纯虚是 `VertexPoolRegistry` 真正需要的那一半:它把异构的源存进
        // `std::array<IVertexSource*, N>`，用 buffer() 写描述符、用 setBindlessPoolId()
        // 分配槽位。kind() / layout() / bindlessPoolId() 当前也没有多态调用点，但它们
        // **没有撒谎**(每个实现都诚实返回自己的值)，且 kind() 的用途是给注册表决定
        // barrier 语义——留给上面文件头写明的第三个规划实现 VirtualGeometrySource。
    };

} // namespace lux::render
