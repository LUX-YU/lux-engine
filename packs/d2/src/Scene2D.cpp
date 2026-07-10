// ============================================================================
//  Scene2D.cpp — the 2D pack's registry entries (ADR §3: capabilities are
//  self-registered ScenePackEntry literals; the old central install() body is
//  gone — adding a capability is adding an entry here, zero central edits).
//
//  Entry order (the canonical system order as DATA):
//    10 SpriteAnimation system        (gameplay pre-step, frame dt)
//    20 Simulation2DSystem            (any fixed-step capability)
//    30 Transform2DSystem   (Core)    — AFTER the simulation step, so fixed-
//    40 Camera2DSystem      (Core)      step writes land in this frame's matrix
//    50 pixel phases        (PixelSimulation → ApplyFieldCommands/SimulateFields)
//    60 interop probe       (PixelInterop → FieldCollisionAdapter published to
//                            the CollisionProbes2D seam; a physics pack DRAINS
//                            it at its own, later order — no solver type here)
//  Bridges: 10 camera upload (Core), 20 sprites, 30 pixel field, 40 tilemap.
// ============================================================================

#include <lux/pack/d2/Scene2D.hpp>
#include <lux/pack/d2/pixel/PixelFieldRuntime.hpp>
#include <lux/pack/d2/pixel/FieldCollisionAdapter.hpp>
#include <lux/pack/d2/CollisionProbe2D.hpp>
#include <lux/pack/d2/world/systems/Simulation2DSystem.hpp>
#include <lux/pack/d2/world/systems/SpriteAnimationSystem.hpp>
#include <lux/pack/d2/world/systems/Transform2DSystem.hpp>
#include <lux/pack/d2/world/systems/Camera2DSystem.hpp>
#include <lux/pack/d2/render_bridge/Sprite2DBridge.hpp>
#include <lux/pack/d2/render_bridge/PixelField2DBridge.hpp>
#include <lux/pack/d2/render_bridge/Camera2DUploadBridge.hpp>
#include <lux/pack/d2/render_bridge/Tilemap2DBridge.hpp>
#include <lux/engine/render_bridge/RenderableSystem.hpp>
#include <lux/engine/ecs/World.hpp>

#include <cassert>
#include <memory>

namespace lux::pack
{
    using lux::ecs::World;
    using lux::render_bridge::RenderableSystem;
    using lux::render_bridge::SceneServices;
    using lux::render_bridge::ScenePackContext;
    using lux::render_bridge::ScenePackEntry;
    using lux::render_bridge::ScenePackRegistry;

    namespace
    {
        [[nodiscard]] const D2ScenePlan& planOf(const ScenePackContext& ctx) noexcept
        {
            return *static_cast<const D2ScenePlan*>(ctx.plan);
        }

        constexpr std::uint32_t bit(D2Capability c) noexcept
        {
            return static_cast<std::uint32_t>(c);
        }
        /// Every capability that needs the shared fixed-step coordinator.
        constexpr std::uint32_t kFixedStepMask =
            bit(D2Capability::PixelSimulation) | bit(D2Capability::Physics) |
            bit(D2Capability::CharacterController) | bit(D2Capability::PixelInterop);
    } // namespace

    void addD2Pack(ScenePackRegistry& reg)
    {
        // 10 — A2-01 gameplay pre-step: samples frame animation BEFORE the
        // bridges extract this frame's sprite state.
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::SpriteAnimation),
            .order        = 10,
            .install      = [](const ScenePackContext& ctx)
            {
                ctx.world->addSystem(std::make_unique<SpriteAnimationSystem>());
            }});

        // 20 — the ONE fixed-step coordinator every fixed-step capability
        // shares (no per-capability accumulator, design §2.4). Published to
        // services so later entries (pixel phases, whichever physics pack the
        // caller registered) wire their phases onto it.
        reg.add(ScenePackEntry{
            .active_on    = kFixedStepMask,   // shared infra: activates on any fixed-step cap, BACKS none
            .order        = 20,
            .install      = [](const ScenePackContext& ctx)
            {
                auto sim = std::make_unique<Simulation2DSystem>(planOf(ctx).fixedStep());
                ctx.services.adopt(sim.get());   // owned by the World
                ctx.world->addSystem(std::move(sim));
            }});

        // 30/40 — Core.
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::Core),
            .order        = 30,
            .install      = [](const ScenePackContext& ctx)
            {
                auto xf = std::make_unique<Transform2DSystem>();
                ctx.services.adopt(xf.get());
                ctx.world->addSystem(std::move(xf));
            }});
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::Core),
            .order        = 40,
            .install      = [](const ScenePackContext& ctx)
            {
                auto camera = std::make_unique<Camera2DSystem>();
                ctx.services.adopt(camera.get());
                ctx.world->addSystem(std::move(camera));
            }});

        // 50 — F2: the pixel-field phases. The runtime is a SERVICE the caller
        // provides (adopt/emplace) — preflighted so a PixelSimulation plan
        // without a runtime refuses BEFORE the World is touched.
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::PixelSimulation),
            .order        = 50,
            .preflight    = [](const ScenePackContext& ctx)
            {
                return ctx.services.get<PixelFieldRuntime>() != nullptr;
            },
            .install      = [](const ScenePackContext& ctx)
            {
                auto* runtime = ctx.services.get<PixelFieldRuntime>();
                auto* sim     = ctx.services.get<Simulation2DSystem>();
                assert(runtime && sim && "order/preflight contract broken");
                sim->setPhase(Simulation2DSystem::Phase::ApplyFieldCommands,
                    [runtime](lux::meta::EntityRegistry& r, float)
                    {
                        runtime->maintainOwners(r);
                        runtime->applyCommands();
                    });
                sim->setPhase(Simulation2DSystem::Phase::SimulateFields,
                    [runtime](lux::meta::EntityRegistry&, float) { runtime->step(); });
            }});

        // 60 — I2-00: publish the field's collision adapter onto the
        // CollisionProbes2D seam. NO solver type appears in this pack:
        // whichever physics pack the caller registered drains the list at its
        // own (later) order.
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::PixelInterop),
            .order        = 60,
            .preflight    = [](const ScenePackContext& ctx)
            {
                return ctx.services.get<PixelFieldRuntime>() != nullptr;
            },
            .install      = [](const ScenePackContext& ctx)
            {
                auto* runtime = ctx.services.get<PixelFieldRuntime>();
                auto& adapter = ctx.services.emplace(
                    std::make_unique<FieldCollisionAdapter>(runtime));
                auto* probes = ctx.services.get<CollisionProbes2D>();
                if (!probes)
                    probes = &ctx.services.emplace(std::make_unique<CollisionProbes2D>());
                probes->probes.push_back(&adapter);
            }});

        // ── bridges (design §0R V1/V2: bespoke IRenderableBridge via addBridge) ──
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::Core),
            .order        = 10,
            .bridges      = [](RenderableSystem& rs, const ScenePackContext&)
            {
                rs.addBridge(std::make_unique<Camera2DUploadBridge>());
            }});
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::SpriteRendering),
            .order        = 20,
            .bridges      = [](RenderableSystem& rs, const ScenePackContext&)
            {
                rs.addBridge(std::make_unique<Sprite2DBridge>());
            }});
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::PixelSimulation),
            .order        = 30,
            .bridges      = [](RenderableSystem& rs, const ScenePackContext& ctx)
            {
                if (auto* runtime = ctx.services.get<PixelFieldRuntime>())
                    rs.addBridge(std::make_unique<PixelField2DBridge>(runtime));
            }});
        reg.add(ScenePackEntry{
            .backs        = bit(D2Capability::SpriteRendering),
            .order        = 40,
            .bridges      = [](RenderableSystem& rs, const ScenePackContext&)
            {
                rs.addBridge(std::make_unique<Tilemap2DBridge>());
            }});
    }

    namespace
    {
        [[nodiscard]] const ScenePackRegistry& d2OnlyRegistry()
        {
            static const ScenePackRegistry reg = []
            {
                ScenePackRegistry r;
                addD2Pack(r);
                return r;
            }();
            return reg;
        }
    } // namespace

    std::uint32_t unbackedCapabilities(const D2ScenePlan& plan,
                                       const ScenePackRegistry& reg) noexcept
    {
        return plan.capabilities() & ~reg.backedMask();
    }

    D2Installed install(World& world, SceneServices& services,
                        const ScenePackRegistry& reg, const D2ScenePlan& plan)
    {
        D2Installed out;
        // Refuse an illegal plan wholesale — never a partial install (D-01/D-02):
        // an invalid plan, a capability nothing registered backs, or a failed
        // entry preflight (e.g. PixelSimulation without a runtime service) all
        // return before ANY entry touched the World.
        if (!plan.validate().ok())
            return out;
        if (unbackedCapabilities(plan, reg) != 0u)
            return out;
        const ScenePackContext ctx{&world, services, &plan};
        if (!reg.installAll(ctx, plan.capabilities()))
            return out;
        out.ok         = true;
        out.simulation = services.get<Simulation2DSystem>();
        out.transform  = services.get<Transform2DSystem>();
        out.camera     = services.get<Camera2DSystem>();
        return out;
    }

    D2Installed install(World& world, SceneServices& services, const D2ScenePlan& plan)
    {
        return install(world, services, d2OnlyRegistry(), plan);
    }

    void registerBridges(RenderableSystem& rs, SceneServices& services,
                         const ScenePackRegistry& reg, const D2ScenePlan& plan)
    {
        if (!plan.validate().ok() || unbackedCapabilities(plan, reg) != 0u)
            return;
        const ScenePackContext ctx{nullptr, services, &plan};   // bridges never touch the World
        reg.registerBridges(rs, ctx, plan.capabilities());
    }

    void registerBridges(RenderableSystem& rs, SceneServices& services, const D2ScenePlan& plan)
    {
        registerBridges(rs, services, d2OnlyRegistry(), plan);
    }

    D2ScenePlan traditional2DPlan()
    {
        // The one preset that is fully installable today: a Camera2D + rendered sprites.
        return D2ScenePlan{}.enableCore().enableSpriteRendering();
    }

} // namespace lux::pack
