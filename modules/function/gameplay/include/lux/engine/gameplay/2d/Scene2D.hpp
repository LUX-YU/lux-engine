#pragma once
// ============================================================================
//  Scene2D.hpp — install entry for the 2D kit (lux::gameplay::d2).
//
//  Mirrors d3::Scene3D.hpp (installSystems / registerRenderables) but PLAN-driven
//  (design §2.5): a D2ScenePlan is turned into ONE deterministic system order in a
//  single addSystem pass, because the append-only World cannot reorder afterwards.
//
//  Two free functions (no Scene2D class), matching the d3 convention:
//    - install()         — add the plan's systems to a World, once, in canonical
//                          order (gameplay pre-step → Simulation2DSystem[opt] →
//                          Transform2D → Camera2D). D-02 fills the ordering as the
//                          systems land; today an empty plan installs nothing.
//    - registerBridges() — register the plan's custom IRenderableBridge set on a
//                          RenderableSystem (Sprite/Tilemap/PixelField — all custom,
//                          NOT the generic INSTANCE/POOL bridges; design §2.2 note).
//
//  PixelFieldRuntime is the scene-scoped field service (design §0R.3): non-owning,
//  injected into the pixel systems + bridge, and MUST outlive them. Pass nullptr for
//  plans without PixelSimulation. It is forward-declared here (defined by a later task).
// ============================================================================

#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::gameplay
{
    class World;
    class RenderableSystem;
}

namespace lux::gameplay::d2
{
    class Simulation2DSystem;
    class Transform2DSystem;
    class Camera2DSystem;

    /// Scene-scoped 2D field service (chunks/cells/materials/commands live here, not
    /// in components). Owned by the scene container, injected non-owning into systems
    /// + the PixelField2DBridge. Defined by a later pixel-field task; forward-declared
    /// so the install API compiles for pixel-less plans (which pass nullptr).
    class PixelFieldRuntime;

    /// Non-owning pointers to the systems install() added — so the host / registerBridges
    /// can wire phase strategies onto them (esp. the Simulation2DSystem's phases). Every
    /// pointer is null when its system was not installed for the plan. The systems are
    /// owned by the World; these are valid only as long as the World is.
    struct D2Installed
    {
        Simulation2DSystem* simulation = nullptr;   ///< the unified fixed-step coordinator (if needsSimulation)
        Transform2DSystem*  transform  = nullptr;   ///< local 2D TRS → world (if has(Core))
        Camera2DSystem*     camera     = nullptr;   ///< world → ortho view/proj (if has(Core))
    };

    /// Install @p plan's systems onto @p world in ONE deterministic addSystem pass, in
    /// canonical order. Call once, right after constructing the World, BEFORE its first
    /// tick(). An empty plan installs nothing (payment symmetry). An INVALID plan
    /// (!plan.validate().ok()) or a PixelSimulation plan given a null @p runtime installs
    /// NOTHING and returns an empty result — never a partial install. @p runtime may be
    /// null unless the plan enables PixelSimulation.
    LUX_FUNCTION_PUBLIC D2Installed install(World& world, PixelFieldRuntime* runtime, const D2ScenePlan& plan);

    /// Register @p plan's custom renderable bridges on @p rs (Sprite2D / Tilemap2D /
    /// PixelField2D — all custom IRenderableBridge via addBridge). Call once, BEFORE
    /// the RenderableSystem's first update(). An empty plan registers nothing.
    LUX_FUNCTION_PUBLIC void registerBridges(RenderableSystem& rs, PixelFieldRuntime* runtime, const D2ScenePlan& plan);

    /// Convenience preset: a side-scrolling platformer (Core + sprite animation +
    /// physics + character controller). Equivalent to chaining those enable*() calls.
    [[nodiscard]] LUX_FUNCTION_PUBLIC D2ScenePlan platformerPlan();

} // namespace lux::gameplay::d2
