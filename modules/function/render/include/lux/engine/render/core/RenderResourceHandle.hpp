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
 *   Allocated by : game thread
 *   Stored in    : ECS components (DrawPartHandle), RenderPrimitive
 *   Read by      : upload thread (fill/markReady), render thread (get/isReady)
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
