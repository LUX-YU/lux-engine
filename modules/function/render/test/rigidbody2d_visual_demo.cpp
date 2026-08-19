// ============================================================================
//  rigidbody2d_visual_demo.cpp — VISUAL (interactive tier): the Box2D-backed
//  Physics2DSystem on screen. Solid-tint image boxes (Collider2D +
//  RigidBody2D) fall under gravity, collide, TIP and STACK into a pile on a
//  static floor between two walls — real rigid-body dynamics the swept demo
//  solver cannot do (box-vs-box, rotation, resting contacts).
//
//  The scene is assembled through the real SceneContribution descriptors.
//  Physics2DSystem is driven by
//  the shared Simulation2DSystem fixed step — nothing bespoke.
//
//  CONTROLS:  Space = drop a box    R = reset the pile    (auto-drops a stack at start)
//
//  Script event registry smoke test: the FLOOR carries a Lua script whose
//  OnCollision2DEnter prints every impact — the visible proof of the chain
//  Box2D begin-touch → Physics2DSystem collision sink → ScriptSystem
//  subscription-index dispatchTo → Lua. Watch the console while boxes land.
//
//  NOT self-checking — for eyeballing. `interactive` tier. Exit 0 without Vulkan.
//  `--soak <seconds>` runs the seeded stack unattended and then enters the
//  normal RAII teardown path; controls still require attended validation.
// ============================================================================

#include "DeviceRenderFixture.hpp"
#include "VisualSoak.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>

#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/scene/SceneFeatureId.hpp>
#include <lux/engine/runtime/packs/spatial2d/Physics2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Presentation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Simulation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Transform2DContribution.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/physics/FixedStepConfig.hpp>
#include <lux/engine/ecs/physics2d/Physics2DConfig.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/physics2d/components/Physics2DComponents.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>

// Smoke test: the floor's Lua collision probe.
#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>
#include <lux/engine/ecs/script/backends/LuaScriptBackend.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/ScriptAsset.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace lux::render;
namespace d2 = lux::ecs;
using lux::window::KeyEnum;
using lux::window::KeyState;

namespace
{
    constexpr std::uint32_t W = 1280, H = 800;

    // Image tint is 0xAABBGGRR (little-endian RGBA — see the demos' texture packing).
    constexpr std::uint32_t kPalette[] = {
        0xFF3B7BE6u, 0xFF4CAF6Au, 0xFF3FC5F0u, 0xFF6A5ACDu,
        0xFF2E8BE6u, 0xFF7AC77Bu, 0xFFB08050u, 0xFF5060D0u};
    constexpr std::size_t kPaletteN = sizeof(kPalette) / sizeof(kPalette[0]);
} // namespace

int main(int argc, char** argv)
{
    std::setbuf(stdout, nullptr);
    lux::rendertest::VisualSoak soak;
    if (!lux::rendertest::VisualSoak::parse(argc, argv, soak))
        return 2;
    std::printf("=== rigidbody2d_visual_demo ===\n"
                "    Space = drop a box, R = reset. Boxes fall, tip and stack (Box2D).\n"
                "    mode: %s\n",
                soak.enabled()
                    ? "soak — exits by itself"
                    : "interactive — close the window to exit");

    lux::rendertest::DeviceRenderFixture fx(W, H, "RigidBody2D — Box2D-backed Physics2DSystem");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("RigidBody2D", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    FeatureCatalog features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};

    // 配置经 services 交给要它的那个条目（批 5：plan 这个中间人没了）。
    // services 必须比 World 活得久 —— adopt 的是借用指针，栈上的这两个正合适。
    d2::FixedStepConfig fixed{};
    fixed.fixed_dt = 1.f / 120.f;             // 120 Hz — smooth stacking contacts
    d2::Physics2DConfig gravity{0.f, -18.f};

    // This executable is the host boundary for the linked component sidecars.
    // Drain registrars before contribution schema validation.
    // reflected component catalogue.
    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    if (!lux::ecs::registerGeneratedComponents(components))
        return 1;

    lux::runtime::SceneContributionCatalog contributions;
    auto transform2d =
        lux::runtime::makeSpatial2DTransformContribution(components);
    auto simulation2d =
        lux::runtime::makeSimulation2DContribution(components);
    auto presentation2d =
        lux::runtime::makePresentation2DContribution(components);
    auto physics2d = lux::runtime::makePhysics2DContribution(components);
    if (!transform2d || !simulation2d || !presentation2d || !physics2d)
        return 1;
    std::vector<lux::runtime::SceneContributionDescriptor> descriptors;
    descriptors.push_back(std::move(*transform2d));
    descriptors.push_back(std::move(*simulation2d));
    descriptors.push_back(std::move(*presentation2d));
    descriptors.push_back(std::move(*physics2d));
    if (!contributions.addBatch(std::move(descriptors)))
        return 1;
    // 驻留三件套(裁决二):声明在渲染绑定之前 —— 逆序析构。
    lux::runtime::testing::AsyncTestServices async(
        assets,
        fx.upload(),
        fx.sync(),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 1}
    );
    if (!async.valid())
        return 1;
    lux::runtime::ResidencyAssembly residency(
        fx.control(),
        async.uploadClient(),
        assets,
        features,
        async.assetClient(),
        async.runtime(),
        {}
    );
    // Registry owner must outlive every observer that detaches from it.
    lux::ecs::World world;
    // 资产归驻留胶水:asset_id 经三件套解析成 GPU 句柄写进 cache 组件。
    // 驻留胶水是一个普通节点(批 B1)。本 demo 手工驱动(不走
    // Schedule::tick),所以 Schedule 自动做的三件事在这里手动做:
    // 连信号、每帧 drain、登记成服务供包的 resolve* 声明取用。
    auto residency_glue = std::make_unique<lux::ecs::ResidencySubsystem>(assets);
    residency_glue->setCallbacks(residency.makeCallbacks());
    auto* const residency_owner = residency_glue.get();
    lux::ecs::RenderSystemBuilder render_builder;
    if (!render_builder.add(std::move(residency_glue)) ||
        !render_builder.add(std::make_unique<lux::ecs::CameraViewSubsystem>()))
    {
        std::printf("Failed to stage base render subsystems.\n");
        return 1;
    }

    // Reverse destruction is intentional: builder → schedule systems → owned
    // services → residency observers → World registry. Longer-lived borrowed
    // render/asset resources remain below that graph.
    lux::ecs::SceneServices   services;
    lux::ecs::Schedule        schedule{world};
    lux::ecs::ScheduleBuilder assembly{schedule, services};

    auto& staged = assembly.services();
    if (!staged.adopt(fixed) ||
        !staged.adopt(gravity) ||
        !staged.adopt(*residency_owner) ||
        !staged.adopt(render_builder))
    {
        std::printf("Failed to stage 2D scene services.\n");
        return 1;
    }

    constexpr std::array selected{
        lux::scene::sceneFeatureId(
            lux::runtime::kPresentation2DContributionName),
        lux::scene::sceneFeatureId(
            lux::runtime::kPhysics2DContributionName)};
    if (!contributions.assembleDefaults(assembly, selected))
    {
        std::printf("scene feature assembly failed\n");
        return 1;
    }
    auto render_plan = std::move(render_builder).compile();
    if (!render_plan)
    {
        std::printf("render subsystem graph failed\n");
        return 1;
    }
    auto render_system = std::make_unique<lux::ecs::RenderSystem>(
        fx.session(),
        fx.control(),
        async.uploadClient(),
        fx.control().adoptScene(sv.scene_id),
        std::move(*render_plan));
    auto* const render_owner = render_system.get();
    render_system->setFeatures(features);
    if (!assembly.add(std::move(render_system), lux::ecs::kPhaseRender))
    {
        std::printf("RenderSystem installation failed\n");
        return 1;
    }
    if (const auto committed = assembly.commit(); !committed)
    {
        std::printf("schedule commit failed\n");
        return 1;
    }

    // 连信号:包的 resolve* 声明此刻已全部注册(Schedule 走 onAdded,
    // 手工驱动就在这里)。
    residency_owner->attach(world.registry());
    if (services.get<d2::Simulation2DSystem>() == nullptr ||
        services.get<d2::Physics2DSystem>() == nullptr)
    { std::printf("install failed\n"); return 1; }

    // ── camera: frames the floor + a tall column above it ──
    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam).position = {0.0, 2.4};
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 6.4f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    world.emplace<lux::ecs::PrimaryCameraTag>(cam);
    world.emplace<lux::ecs::RenderViewBindingComponent>(cam,
        fx.control().adoptView(sv.scene_id, sv.view));

    // ── static geometry: floor + two side walls (Collider2D + a tint image,
    //    same numbers, so the visual and the physical body cannot drift) ──
    const auto addStatic = [&](Eigen::Vector2f center, Eigen::Vector2f half, std::uint32_t tint)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>(center.x()),
            static_cast<double>(center.y())};
        world.emplace<d2::Collider2DComponent>(e).half_extents = half;
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        sp.size = half * 2.f;
        sp.tint = tint;
        sp.priority = 0.f;
        return e;
    };
    const auto floor_entity =
        addStatic({0.f, -0.25f}, {3.2f, 0.25f}, 0xFF3A3A3Au);   // floor (top at y=0)
    addStatic({-3.0f, 2.0f}, {0.2f, 2.5f}, 0xFF303030u);        // left wall
    addStatic({ 3.0f, 2.0f}, {0.2f, 2.5f}, 0xFF303030u);        // right wall

    // ── dynamic bodies ──
    std::vector<lux::meta::entity_id> boxes;
    int spawn_count = 0;
    const auto dropBox = [&](float x, float y)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>(x), static_cast<double>(y)};
        // slight size variety so the pile is not a perfect lattice
        const float s = 0.18f + 0.06f * static_cast<float>((spawn_count * 7) % 5) / 5.f;
        world.emplace<d2::Collider2DComponent>(e).half_extents = Eigen::Vector2f(s, s);
        world.emplace<d2::RigidBody2DComponent>(e);
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        sp.size = Eigen::Vector2f(s * 2.f, s * 2.f);
        sp.tint = kPalette[static_cast<std::size_t>(spawn_count) % kPaletteN];
        sp.priority = 5.f;
        boxes.push_back(e);
        ++spawn_count;
    };
    const auto resetPile = [&]
    {
        for (const auto e : boxes)
            if (world.registry().valid(e))
                world.registry().destroy(e);
        boxes.clear();
    };
    const auto seedStack = [&]
    {
        // a loose column with x-jitter → it topples into a natural pile
        for (int i = 0; i < 14; ++i)
            dropBox(0.35f * std::sin(static_cast<float>(i) * 1.3f), 1.2f + 0.55f * i);
    };
    seedStack();


    // ── Smoke test: a Lua collision probe on the FLOOR ─────────────────────
    // Every box landing prints one line — the whole event chain on screen:
    // Box2D → collision sink → subscription-index dispatchTo → Lua.
    d2::scriptRegistry().registerBackend(
        std::make_unique<d2::LuaScriptBackend>(components)
    );
    {
        lux::rdesc::Script desc;
        desc.module_name = "floor_collision_probe";
        desc.body        = lux::rdesc::LuaSourceScript{};
        auto data  = std::make_unique<lux::rdesc::Script>(std::move(desc));
        auto asset = assets.createAsset<lux::asset::ScriptAsset>(std::move(data));
        const auto script_id = asset->id();
        const char* src = R"lua(
            return {
                OnCollision2DEnter = function(self, other)
                    print("[lua] collision enter: floor(" .. self:id() ..
                          ") <- box(" .. other .. ")")
                end,
            }
        )lua";
        auto* sa = static_cast<lux::asset::ScriptAsset*>(asset.get());
        std::vector<std::byte> bytes(std::strlen(src));
        std::memcpy(bytes.data(), src, bytes.size());
        sa->setPayload(std::move(bytes));
        assets.registerAsset(std::move(asset));
        world.emplace<d2::ScriptComponent>(floor_entity).script = script_id;
    }
    d2::ScriptContext script_ctx;
    script_ctx.world  = &world;
    script_ctx.assets = &assets;
    d2::ScriptSystem scripts(d2::scriptRegistry(), script_ctx);
    scripts.onRuntimeStart(world.registry());
    if (auto* physics = services.get<d2::Physics2DSystem>())
    {
        const auto ev = d2::scriptEventRegistry().find("OnCollision2DEnter");
        physics->setCollisionSink(
            [&world, &scripts, ev](lux::meta::entity_id self,
                                   lux::meta::entity_id other)
            {
                std::uint32_t o = static_cast<std::uint32_t>(other);
                void* args[1]  = { &o };
                scripts.dispatchTo(world.registry(), self, ev, args);
            });
    }

    std::printf("dropped %d boxes. Space = more, R = reset.\n"
                "floor carries a Lua OnCollision2DEnter probe — watch for "
                "[lua] lines as boxes land.\n", spawn_count);

    const auto t0   = std::chrono::steady_clock::now();
    auto last_time  = t0;
    auto fps_mark   = t0;
    int  fps_frames = 0;
    std::uint64_t total_frames = 0;
    bool prev_space = false, prev_r = false;

    while (fx.running())
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;

        // ── input (edge-detected so a held key fires once) ──
        const bool space = fx.window().queryKey(KeyEnum::KEY_SPACE) == KeyState::PRESS;
        const bool r     = fx.window().queryKey(KeyEnum::KEY_R) == KeyState::PRESS;
        if (space && !prev_space)
            dropBox(0.4f * std::sin(static_cast<float>(spawn_count)), 4.6f);
        if (r && !prev_r)
        {
            resetPile();
            seedStack();
        }
        prev_space = space;
        prev_r = r;

        async.drainMainThreadCompletions();              // 驻留管道主线程会合
        schedule.tick(dt);              // fixed-step Simulation2DSystem → Physics2DSystem
        scripts.update({world.registry(), dt});   // OnUpdate dispatch (probe implements none — cheap)
        residency_owner->drainResolvers(world.registry());
        fx.flush();

        ++total_frames;
        ++fps_frames;
        const auto soak_now = std::chrono::steady_clock::now();
        if (soak.reached(t0, soak_now))
        {
            soak.reportGracefulTeardown(t0, soak_now, total_frames);
            break;
        }
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            float lowest = 99.f;
            for (const auto e : boxes)
                if (world.registry().valid(e))
                    lowest = std::min(
                        lowest,
                        static_cast<float>(
                            world.registry()
                                .get<d2::Transform2DComponent>(e)
                                .position.y));
            std::printf("fps=%d | boxes=%zu | lowest_y=%.2f\n", fps_frames, boxes.size(), lowest);
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    auto render_close = render_owner->close();
    while (render_close == lux::render::ERenderLeaseCloseStatus::Stopping)
    {
        if (!fx.control().waitAndPumpReplies())
            return 1;
        fx.pumpReplies();
        render_close = render_owner->close();
    }
    const auto residency_close =
        lux::runtime::testing::detail::closeResidency(
            residency,
            async.runtime());
    if (!residency_close.clean())
        return 1;
    async.close();
    return 0;
}
