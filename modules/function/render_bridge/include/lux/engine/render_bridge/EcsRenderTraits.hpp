#pragma once
/**
 * @file EcsRenderTraits.hpp
 * @brief Per-component declaration of "how this ECS component bridges to render".
 *
 * Replaces the per-component hand-written bridge (IRenderableBridge subclass). Each renderable
 * component specializes `EcsRenderTraits<C>`, declaring its lifecycle KIND + the few
 * component-specific hooks; three generic bridge templates (ParamBridge / PoolBridge /
 * InstanceBridge) carry the invariant lifecycle (view loop + dirty-diff + CRUD + reap)
 * once. The component talks DIRECTLY to its render proxy through the traits — no
 * intermediate IR. Register via `RenderableSystem::registerComponent<C>()`.
 *
 * Three kinds (see the design doc renderable-traits-design.md §2 for full hook tables):
 *   PARAM    — component fields → a feature SetParams op, dirty-diffed (Grid, Skybox).
 *   POOL     — component → create/update/destroy in a render pool, async handle (Lights).
 *   INSTANCE — component → GPU mesh instance: asset ensure/refcount/swap/reap (Meshes).
 *
 * The primary template is intentionally UNDEFINED: a component with no specialization
 * is "not renderable", a compile-time fact (instantiating a bridge for it fails).
 */

#include <cstdint>

#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entt::exclude

namespace lux::render_bridge
{
    /// Which generic bridge drives this component's lifecycle.
    enum class ERenderableKind : std::uint8_t { PARAM, POOL, INSTANCE };

    /// Declarative companion/exclusion component list for POOL/INSTANCE traits — the
    /// bridge expands it into `registry.view<C, Require...>(entt::exclude<Exclude...>)`.
    template <class... Cs> struct ComponentList {};

    /// Build the entt view for a primary component C plus its traits' Require/Exclude
    /// lists. POOL/INSTANCE bridges call it as `componentView<C>(reg, Require{}, Exclude{})`.
    template <class C, class... Rs, class... Es>
    [[nodiscard]] inline auto componentView(lux::meta::EntityRegistry& reg, ComponentList<Rs...>, ComponentList<Es...>)
    {
        return reg.template view<C, Rs...>(entt::exclude<Es...>);
    }

    /// Membership predicate for the same `componentView<C>(reg, Require, Exclude)` set,
    /// queried per-entity. The INSTANCE bridge's reap uses it to tear down any instance
    /// whose entity left the view — died, shed C/Require, or gained an Exclude tag (e.g.
    /// world-streaming's dormant tag). The bridge therefore never names a specific
    /// exclusion component; the trait's Exclude list is the single source of truth.
    template <class C, class... Rs, class... Es>
    [[nodiscard]] inline bool inComponentView(lux::meta::EntityRegistry& reg, lux::meta::entity_id e, ComponentList<Rs...>, ComponentList<Es...>)
    {
        if (!reg.valid(e) || !reg.template all_of<C, Rs...>(e)) return false;
        if constexpr (sizeof...(Es) > 0)
            if (reg.template any_of<Es...>(e)) return false;
        return true;
    }

    /// Zero-copy world transform handed to INSTANCE bridges: `world` points into the
    /// component's own column-major 4x4 storage (Eigen Matrix4f is contiguous and its
    /// memory layout matches the renderer's std430 mat4 directly — no staging array).
    /// `dirty` gates the per-frame `updateTransform`. The pointer is valid only for the
    /// frame it is fetched in (the component is borrowed, not copied).
    struct InstanceTransform
    {
        const float* world{nullptr};
        bool         dirty{false};
    };

    /// Specialize this per renderable component. No primary definition on purpose.
    template <class C> struct EcsRenderTraits;

} // namespace lux::render_bridge
