// ============================================================================
//  Scene2D.cpp — install entry impl for the 2D kit (lux::gameplay::d2).
//
//  install() adds the plan's systems to the World in ONE deterministic pass (the
//  append-only World cannot reorder afterwards): an optional fixed-step
//  Simulation2DSystem, then Transform2DSystem and Camera2DSystem for Core. An empty
//  plan installs nothing. registerBridges() adds the matching render producers.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelFieldRuntime.hpp>
#include <lux/engine/gameplay/2d/world/systems/Simulation2DSystem.hpp>
#include <lux/engine/gameplay/2d/world/systems/SpriteAnimationSystem.hpp>
#include <lux/engine/gameplay/2d/physics/Physics2DWorld.hpp>
#include <lux/engine/gameplay/2d/pixel/FieldCollisionAdapter.hpp>   // I2-00
#include <lux/engine/gameplay/2d/world/systems/Transform2DSystem.hpp>
#include <lux/engine/gameplay/2d/world/systems/Camera2DSystem.hpp>
#include <lux/engine/gameplay/2d/render_bridge/Sprite2DBridge.hpp>
#include <lux/engine/gameplay/2d/render_bridge/PixelField2DBridge.hpp>
#include <lux/engine/gameplay/2d/render_bridge/Camera2DUploadBridge.hpp>
#include <lux/engine/gameplay/2d/render_bridge/Tilemap2DBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>   // addBridge
#include <lux/engine/gameplay/world/World.hpp>

#include <memory>

namespace lux::gameplay::d2
{
    namespace
    {
        // ── The capability-backing table ────────────────────────────────────
        // A capability may be ENABLED only once a slice has landed the system/phase
        // that backs it — otherwise validate() passes, install() succeeds, and the
        // caller gets a silently dead capability (the exact trap the platformerPlan
        // removal banned for presets, one level down). Each slice extends this mask
        // IN THE SAME CHANGE that wires its backing (F2: PixelSimulation via
        // Phase::SimulateFields; P2: Physics/CharacterController; A2: SpriteAnimation),
        // so the promise and the implementation cannot drift apart.
        constexpr std::uint32_t kBackedCapabilities =
            static_cast<std::uint32_t>(D2Capability::Core) |
            static_cast<std::uint32_t>(D2Capability::SpriteRendering) |
            static_cast<std::uint32_t>(D2Capability::PixelSimulation) |  // F2: backed below
            static_cast<std::uint32_t>(D2Capability::SpriteAnimation) |  // A2-01: backed below
            static_cast<std::uint32_t>(D2Capability::Physics) |          // P2: backed below
            static_cast<std::uint32_t>(D2Capability::CharacterController) |
            static_cast<std::uint32_t>(D2Capability::PixelInterop);     // I2: backed below
    } // namespace

    std::uint32_t unbackedCapabilities(const D2ScenePlan& plan) noexcept
    {
        return plan.capabilities() & ~kBackedCapabilities;
    }

    D2Installed install(World& world, PixelFieldRuntime* runtime, const D2ScenePlan& plan)
    {
        D2Installed installed;

        // Refuse an illegal plan wholesale — never a partial install (D-01/D-02). A
        // PixelSimulation plan additionally needs a live field runtime; that runtime-
        // presence check belongs here (validate() is plan-only). A plan enabling a
        // capability nothing backs yet is refused the same way (see table above).
        if (!plan.validate().ok())
            return installed;
        if (unbackedCapabilities(plan) != 0u)
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

        // 1. SpriteAnimationSystem (A2-01) — the gameplay pre-step: samples frame
        //    animation on frame dt and writes SpriteComponent uv/pivot BEFORE the
        //    bridges extract this frame's sprite state. Asset resolution is NOT
        //    here — the app drives SpriteAnim2DResolver before World::tick.
        if (plan.has(D2Capability::SpriteAnimation))
            world.addSystem(std::make_unique<SpriteAnimationSystem>());

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

        // P2: back the Physics/CharacterController capabilities — one shared
        // Physics2DWorld, stepped by the SimulatePhysics phase. It writes
        // Transform2D BEFORE Transform2DSystem composes the world matrix (the
        // canonical order above), so there is no one-frame transform lag.
        if (plan.has(D2Capability::Physics) && installed.simulation != nullptr)
        {
            auto physics = std::make_shared<Physics2DWorld>(plan.physicsConfig());
            installed.physics = physics.get();

            // I2-00: back the PixelInterop capability — pixel terrain becomes
            // SOLID to the controller by registering the field probe on the
            // physics world. The Field↔Entity TRANSFER half of the capability
            // is the runtime's caller-driven API (prepareExtract/commitExtract
            // + StampCells commands), invoked by game systems inside the
            // FieldToEntity / CollectEntityToField phases — install wires no
            // default transfer behaviour (what to extract is game logic).
            // validate() guarantees Physics ∧ PixelSimulation here.
            std::shared_ptr<FieldCollisionAdapter> field_probe;
            if (plan.has(D2Capability::PixelInterop) && runtime != nullptr)
            {
                field_probe = std::make_shared<FieldCollisionAdapter>(runtime);
                physics->addProbe(field_probe.get());
            }

            installed.simulation->setPhase(Simulation2DSystem::Phase::SimulatePhysics,
                [physics, field_probe](lux::meta::EntityRegistry& reg, float dt)
                {
                    physics->step(reg, dt);
                });
        }

        // F2: back the PixelSimulation capability — the runtime is captured into the
        // coordinator's canonical phases (non-null guaranteed by the gate above).
        // ApplyFieldCommands also carries the owner maintenance (value-scan reap of
        // fields whose owning component/entity died), so ownership and commands are
        // settled BEFORE the CA scan of the same substep.
        if (plan.has(D2Capability::PixelSimulation) && installed.simulation != nullptr)
        {
            installed.simulation->setPhase(Simulation2DSystem::Phase::ApplyFieldCommands,
                [runtime](lux::meta::EntityRegistry& reg, float)
                {
                    runtime->maintainOwners(reg);
                    runtime->applyCommands();
                });
            installed.simulation->setPhase(Simulation2DSystem::Phase::SimulateFields,
                [runtime](lux::meta::EntityRegistry&, float) { runtime->step(); });
        }
        return installed;
    }

    void registerBridges(RenderableSystem& rs, PixelFieldRuntime* runtime, const D2ScenePlan& plan)
    {
        // Custom IRenderableBridge set (design §0R V1/V2 — bespoke bridges via addBridge,
        // NOT the generic INSTANCE/POOL). Registered per enabled capability, BEFORE the
        // first RenderableSystem::update(). Each no-ops if its target render feature is
        // absent, so registering costs only the per-frame view iteration.
        // Camera publish (Core): upload the active Camera2D's ortho view/proj into the
        // per-view ViewGpuData (set 0 binding 1) that every 2D producer's shader reads.
        if (plan.has(D2Capability::Core))
            rs.addBridge(std::make_unique<Camera2DUploadBridge>());

        // Sprite producer — ONLY when SpriteRendering is enabled, so a pixel-only scene
        // never registers (and never per-frame iterates) the sprite view (payment symmetry).
        // v2: RETAINED bridge (one GPU-resident instance per sprite entity, delta ops).
        if (plan.has(D2Capability::SpriteRendering))
            rs.addBridge(std::make_unique<Sprite2DBridge>());

        // PixelField producer (F2-08) — retained: persistent id-mirror texture +
        // canvas field instance per field entity; content rides the export ticket.
        if (plan.has(D2Capability::PixelSimulation) && runtime != nullptr)
            rs.addBridge(std::make_unique<PixelField2DBridge>(runtime));

        // Tilemap2D producer (A2-02) — rides the SpriteRendering capability (no
        // extra plan bit: a tilemap is traditional-2D visuals; payment symmetry
        // holds per entity — no TilemapComponent, no cost beyond the empty view).
        if (plan.has(D2Capability::SpriteRendering))
            rs.addBridge(std::make_unique<Tilemap2DBridge>());
    }

    D2ScenePlan traditional2DPlan()
    {
        // The one preset that is fully installable today: a Camera2D + rendered sprites.
        // Presets for physics / character-control / pixel plans arrive only once those
        // capabilities actually install something — a preset must not promise a capability
        // the install path silently drops.
        return D2ScenePlan{}.enableCore().enableSpriteRendering();
    }

} // namespace lux::gameplay::d2
