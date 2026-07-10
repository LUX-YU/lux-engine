#pragma once
// ============================================================================
//  Scene2D.hpp — install entry for the 2D pack (lux::pack).
//
//  Registry-driven (ADR: lux-engine-pack-architecture §3/§4): the pack's
//  capabilities are ENTRIES on a ScenePackRegistry (addD2Pack); install() is a
//  thin front — validate the plan, refuse anything the assembled registry does
//  not back, then run the entries in canonical order. Concrete runtimes travel
//  through SceneServices, never through signatures:
//
//      render_bridge::SceneServices services;          // before the World!
//      ecs::World world;
//      services.adopt(&runtime);                       // PixelSimulation plans
//      auto installed = pack::install(world, services, plan);       // d2 only
//
//  Plans that enable EXTERNALLY-backed capabilities (Physics — the engine
//  ships no solver, ADR v3) assemble the registry themselves:
//
//      render_bridge::ScenePackRegistry reg;
//      pack::addD2Pack(reg);
//      pack::addPhysics2DDemoPack(reg);                // or a real integration
//      auto installed = pack::install(world, services, reg, plan);
//
//  The physics service (when a physics pack is registered) is fetched from
//  services — D2Installed no longer names any solver type.
// ============================================================================

#include <lux/pack/d2/D2ScenePlan.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render_bridge/ScenePackRegistry.hpp>

namespace lux::ecs           { class World; }
namespace lux::render_bridge { class RenderableSystem; }

namespace lux::pack
{
    class Simulation2DSystem;
    class Transform2DSystem;
    class Camera2DSystem;
    class PixelFieldRuntime;

    /// Non-owning pointers to the systems install() added (all owned by the
    /// World; valid while it lives). Null when not installed for the plan.
    struct D2Installed
    {
        bool                ok         = false;     ///< false = plan refused, World untouched
        Simulation2DSystem* simulation = nullptr;   ///< the unified fixed-step coordinator
        Transform2DSystem*  transform  = nullptr;   ///< local 2D TRS → world (if has(Core))
        Camera2DSystem*     camera     = nullptr;   ///< world → ortho view/proj (if has(Core))
    };

    /// Register the 2D pack's capability entries (systems, phases, bridges)
    /// on @p reg. Backs: Core / SpriteRendering / SpriteAnimation /
    /// PixelSimulation / PixelInterop. Physics is NOT backed here — a physics
    /// pack registers itself (the CollisionProbes2D seam carries the probes).
    LUX_FUNCTION_PUBLIC void addD2Pack(lux::render_bridge::ScenePackRegistry& reg);

    /// Capabilities of @p plan that are enabled but not backed by @p reg.
    /// Non-zero → install() refuses the plan wholesale (never a partial
    /// install; never a silently dead capability).
    [[nodiscard]] LUX_FUNCTION_PUBLIC std::uint32_t unbackedCapabilities(
        const D2ScenePlan& plan, const lux::render_bridge::ScenePackRegistry& reg) noexcept;

    /// Install @p plan's systems onto @p world in ONE deterministic pass, in
    /// registry order. Refusals (invalid plan / unbacked capability / failed
    /// preflight, e.g. PixelSimulation without a PixelFieldRuntime service)
    /// return ok=false with the World untouched. @p services must OUTLIVE the
    /// World (see SceneServices.hpp).
    LUX_FUNCTION_PUBLIC D2Installed install(lux::ecs::World& world,
                                            lux::render_bridge::SceneServices& services,
                                            const lux::render_bridge::ScenePackRegistry& reg,
                                            const D2ScenePlan& plan);

    /// Convenience: install with the d2 pack alone (no external packs).
    LUX_FUNCTION_PUBLIC D2Installed install(lux::ecs::World& world,
                                            lux::render_bridge::SceneServices& services,
                                            const D2ScenePlan& plan);

    /// Register the active entries' renderable bridges on @p rs. Call once,
    /// BEFORE the RenderableSystem's first update().
    LUX_FUNCTION_PUBLIC void registerBridges(lux::render_bridge::RenderableSystem& rs,
                                             lux::render_bridge::SceneServices& services,
                                             const lux::render_bridge::ScenePackRegistry& reg,
                                             const D2ScenePlan& plan);

    /// Convenience: bridges with the d2 pack alone.
    LUX_FUNCTION_PUBLIC void registerBridges(lux::render_bridge::RenderableSystem& rs,
                                             lux::render_bridge::SceneServices& services,
                                             const D2ScenePlan& plan);

    /// Convenience preset for traditional 2D: a Camera2D + rendered sprites
    /// (Core + SpriteRendering) — the capability set that fully installs today.
    [[nodiscard]] LUX_FUNCTION_PUBLIC D2ScenePlan traditional2DPlan();

} // namespace lux::pack
