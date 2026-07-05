// ============================================================================
//  Scene2D.cpp — install entry impl for the 2D kit (lux::gameplay::d2).
//
//  D-00 scaffold: the entry points exist + compile + install NOTHING by default
//  (an empty plan leaves the World system-free — the compile-test's contract).
//  The deterministic order below is documented now; each system is wired in as it
//  lands (Simulation2DSystem in D-03, Transform2D/Camera2D in Slice A), by D-02.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/world/systems/Simulation2DSystem.hpp>
#include <lux/engine/gameplay/2d/world/systems/Transform2DSystem.hpp>
#include <lux/engine/gameplay/2d/world/systems/Camera2DSystem.hpp>
#include <lux/engine/gameplay/2d/render_bridge/Sprite2DBridge.hpp>
#include <lux/engine/gameplay/2d/render_bridge/Camera2DUploadBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>   // addBridge
#include <lux/engine/gameplay/world/World.hpp>

#include <memory>

namespace lux::gameplay::d2
{
    D2Installed install(World& world, PixelFieldRuntime* runtime, const D2ScenePlan& plan)
    {
        D2Installed installed;

        // Refuse an illegal plan wholesale — never a partial install (D-01/D-02). A
        // PixelSimulation plan additionally needs a live field runtime; that runtime-
        // presence check belongs here (validate() is plan-only).
        if (!plan.validate().ok())
            return installed;
        if (plan.has(D2Capability::PixelSimulation) && runtime == nullptr)
            return installed;

        // Canonical order (design §2.5), added in ONE addSystem pass — the append-only
        // World cannot reorder afterwards, so the ORDER is decided here, once. Each
        // system is guarded by its capability; exactly ONE Simulation2DSystem carries
        // ALL fixed-step capabilities (no per-capability accumulator, no double install).
        //
        //   1. gameplay pre-step (SpriteAnimSystem)          — has(SpriteAnimation)  [Slice A]
        //   2. Simulation2DSystem (unified fixed-step)        — needsSimulation()     [here]
        //   3. Transform2DSystem  (local TRS → world)         — has(Core)             [Slice A]
        //   4. Camera2DSystem     (world → ortho view/proj)   — has(Core)             [Slice A]

        // 1. SpriteAnimSystem — Slice A.

        // 2. The unified fixed-step coordinator. Its phase strategies are wired by
        //    registerBridges() / later tasks onto installed.simulation.
        if (plan.needsSimulation())
        {
            auto sim = std::make_unique<Simulation2DSystem>(plan.fixedStep());
            installed.simulation = sim.get();
            world.addSystem(std::move(sim));
        }

        // 3. Transform2DSystem — after the simulation step, so physics/pixel writes to
        //    Transform2D land BEFORE the world matrix is composed (the one-pass order
        //    guarantees it; this is exactly why 2D can't reuse d3's Transform-first order).
        if (plan.has(D2Capability::Core))
        {
            auto xf = std::make_unique<Transform2DSystem>();
            installed.transform = xf.get();
            world.addSystem(std::move(xf));
        }

        // 4. Camera2DSystem — after Transform2D (it reads the composed WorldTransform2D
        //    to derive the ortho view/proj into Camera2DCache).
        if (plan.has(D2Capability::Core))
        {
            auto camera = std::make_unique<Camera2DSystem>();
            installed.camera = camera.get();
            world.addSystem(std::move(camera));
        }

        (void)runtime;   // captured into the sim phases by registerBridges (later)
        return installed;
    }

    void registerBridges(RenderableSystem& rs, PixelFieldRuntime* runtime, const D2ScenePlan& plan)
    {
        // Custom IRenderableBridge set (design §0R V1/V2 — bespoke bridges via addBridge,
        // NOT the generic INSTANCE/POOL). Registered per enabled capability, BEFORE the
        // first RenderableSystem::update(). Each no-ops if its target render feature is
        // absent, so registering costs only the per-frame view iteration.
        if (plan.has(D2Capability::Core))
        {
            // Camera FIRST: upload the active Camera2D's ortho view/proj into the per-view
            // ViewGpuData (set 0 binding 1) that the sprite shader reads.
            rs.addBridge(std::make_unique<Camera2DUploadBridge>());
            // Sprite producer (producer_order 0 — the first 2D producer; tilemap/pixel
            // producers take later ids when their slices land).
            rs.addBridge(std::make_unique<Sprite2DBridge>(/*producer_order=*/0u));
        }

        // Tilemap2D / PixelField2D producers — their slices (A2-xx / F2-xx).
        (void)runtime;
    }

    D2ScenePlan platformerPlan()
    {
        return D2ScenePlan{}
            .enableCore()
            .enableSpriteAnimation()
            .enablePhysics()
            .enableCharacterController();
    }

} // namespace lux::gameplay::d2
