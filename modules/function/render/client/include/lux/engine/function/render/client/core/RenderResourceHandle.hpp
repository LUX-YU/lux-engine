#pragma once
/**
 * @file RenderResourceHandle.hpp
 * @brief Typed, lightweight handles for GPU resources.
 *
 * All handle types are aliases to lux::cxx::SlotKey<Tag>, providing
 * compile-time type safety so a mesh handle can never be accidentally
 * passed where a texture handle is expected.
 *
 * Design notes
 * ------------
 *   - No Vulkan headers, no asset headers — safe to use in ECS
 *     components, game-thread code, and render-thread code alike.
 *   - `gen` provides ABA protection: when a pool slot is released and
 *     later re-allocated, `gen` is bumped so stale handles compare
 *     unequal and fail pool validity checks.
 *   - Default-constructed handles are invalid (is_null() == true).
 *
 * Thread ownership
 * ----------------
 *   句柄本身是 8 字节的 POD,拷到哪个线程都无所谓;下面记的是**谁在动它指向的
 *   那条记录**。
 *
 *   分配 / 就绪标记 : 渲染(服务端)线程。逐条核实过 —— markReady 的三个调用点
 *                     全在渲染线程:RenderServer.cpp 的 finalizeMeshCompletion、
 *                     MeshResources.cpp 的 recordStagingCopies 与 submitTransfers。
 *   存放           : ECS 组件(DrawPartHandle)、RenderPrimitive
 *   读取           : 渲染线程(get/isReady)
 *
 *   ⚠️ 这里曾写着 "Allocated by: game thread / Read by: upload thread
 *   (fill/markReady)"。**两条都不成立**:没有游戏线程碰它,upload worker 也从不
 *   调 markReady —— worker 拿到的是裸 VkBuffer + 偏移 + 一个数据持有者,做完
 *   staging 拷贝就把完成项投回渲染线程,由渲染线程标记就绪。
 */

#include <lux/cxx/container/SlotMap.hpp>

#include <functional>

namespace lux::render
{
    // =========================================================================
    //  Type tags — empty structs used solely to distinguish handle types.
    // =========================================================================
    struct MeshTag{};
    struct MaterialTag{};
    struct TextureTag{};
    struct LightTag{};
    struct AABBHandleTag{};

    // =========================================================================
    //  RenderResourceHandle<Tag> — alias to lux::cxx::SlotKey<Tag>
    // =========================================================================

    template <typename Tag>
    using RenderResourceHandle = lux::cxx::SlotKey<Tag>;

    // =========================================================================
    //  Concrete handle aliases
    // =========================================================================
    using RMeshHandle     = RenderResourceHandle<MeshTag>;
    using RMaterialHandle = RenderResourceHandle<MaterialTag>;
    using RTextureHandle  = RenderResourceHandle<TextureTag>;
    using RLightHandle    = RenderResourceHandle<LightTag>;
    using RAABBHandle     = RenderResourceHandle<AABBHandleTag>;

    // =========================================================================
    //  跨线句柄 ↔ 内部句柄 的转换
    // =========================================================================
    /// R*Handle(跨线协议面)与 *Handle(引擎内部)是两套 tag 不同、布局相同的
    /// 句柄。它们**都是 L0 类型**,所以这个转换器也属于 L0。
    ///
    /// 它此前住在 L5 的**私有** RenderServerImpl.hpp 里 —— 于是每个只想转个
    /// 句柄的装配 TU 都得 include 整个服务端 Impl 头,把 Impl 的每个字段一并
    /// 拉进自己的可见范围。一个三行的类型工具不该有这种影响半径。
    template<typename To, typename From>
    constexpr To handle_cast(From h) noexcept { return To{h.index, h.gen}; }

} // namespace lux::render

// =============================================================================
//  std::hash specialisation — delegates to SlotKey::Hash
// =============================================================================
namespace std
{
    template <typename Tag, typename I, typename G>
    struct hash<lux::cxx::SlotKey<Tag, I, G>>
    {
        size_t operator()(const lux::cxx::SlotKey<Tag, I, G>& h) const noexcept
        {
            return typename lux::cxx::SlotKey<Tag, I, G>::Hash{}(h);
        }
    };
} // namespace std
