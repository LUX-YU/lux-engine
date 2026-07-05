// ============================================================================
//  canvas2d_feature_lifecycle_test.cpp — R2-01 (gpu tier): Canvas2DFeature
//  attach / SinglePerScene-reject / observe-resources / detach-and-reinstall,
//  driven over a real RenderSession command channel against a headless render
//  server (readback_test's window+server harness — skips cleanly if no Vulkan).
//
//  Proves R2-01's acceptance:
//   - registerFeatureType(kCanvas2DFeatureFactory) succeeds → the factory's
//     dynamic op registration (register_ops_fn) runs.
//   - a first addFeature(Canvas2D) attaches; a SECOND is rejected (SinglePerScene).
//   - a server-side probe (attached AFTER Canvas2D) sees Canvas2DResources present
//     and NO 3D mesh arena (InstanceResources absent) — the 2D↔3D decoupling.
//   - removeFeature detaches cleanly and Canvas2D is re-installable.
//
//  Self-checking: 0 = PASS / skip (no Vulkan), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/FeatureDescriptor.hpp>

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>   // kCanvas2DFeatureFactory
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DResources.hpp>    // find<Canvas2DResources>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>               // find<InstanceResources> (3D mesh arena)

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace lux::render;

namespace
{
    // Server-side probe: at its own attach (which runs on the server thread), record
    // whether the scene has a 3D mesh arena (InstanceResources) and the Canvas2D data
    // resource. Installed AFTER Canvas2D so it observes the post-Canvas registry state.
    struct SceneProbeFeature : RenderFeature
    {
        static std::atomic<int> instance_res_absent;   // 1 = no 3D mesh arena
        static std::atomic<int> canvas_res_present;    // 1 = Canvas2DResources present

        SceneProbeFeature() : RenderFeature(RenderFeature::Config{"SceneProbe"}) {}

        lux::render::Expected<void> initAndAttachTo(RenderScene& sc) override
        {
            instance_res_absent.store(sc.sceneRegistry().find<InstanceResources>() == nullptr ? 1 : 0,
                                      std::memory_order_relaxed);
            canvas_res_present.store(sc.sceneRegistry().find<Canvas2DResources>() != nullptr ? 1 : 0,
                                     std::memory_order_relaxed);
            return {};
        }
        void addPasses(RGBuilder& /*builder*/) override {}
    };
    std::atomic<int> SceneProbeFeature::instance_res_absent{-1};
    std::atomic<int> SceneProbeFeature::canvas_res_present{-1};

    FeatureHandle probeCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<SceneProbeFeature>();
    }
    constexpr FeatureDescriptor kProbeDesc{
        .type = featureId("lux.test.canvas2d.probe.v1"),
        .name = "SceneProbe",
    };
    FeatureFactory kProbeFactory() { return makeSimpleFactory(&probeCreateFn, "SceneProbe", kProbeDesc); }

    struct EmptyConfig {};   // trivially-copyable empty config (create_fn ignores params)

    std::vector<const char*> getVulkanExtensions()
    {
        glfwInit();
        std::uint32_t count = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&count);
        std::vector<const char*> result;
        for (std::uint32_t i = 0; i < count; ++i) result.emplace_back(exts[i]);
        return result;
    }

    template <class T>
    T await(RenderSession& s, lux::window::LuxWindow& w, RenderRequest<T> req)
    {
        s.submitFrame(/*blocking=*/true);
        while (!req.isReady()) { w.pollEvents(); s.waitAndPumpReplies(); }
        T r = req.result();
        s.beginFrame({});
        return r;
    }

    void flush(RenderSession& s, lux::window::LuxWindow& w)
    {
        s.submitFrame(/*blocking=*/true);
        w.pollEvents();
        s.waitAndPumpReplies();
        s.beginFrame({});
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== canvas2d_feature_lifecycle_test (R2-01) ===\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(64, 64, "canvas2d_feature_lifecycle_test");

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r)
        { failed.store(true); ready.store(true); return; }
        if (auto r = server.attachToWindow(window); !r)
        { failed.store(true); ready.store(true); return; }
        ready.store(true);
        try { while (server.tick()) {} } catch (...) {}
    });

    while (!ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (failed.load(std::memory_order_acquire))
    {
        std::cerr << "Server init failed (no Vulkan device?). Skipping.\n";
        sync->requestStop(); server_thread.join();
        return 0; // skip
    }

    RenderSession session(channel, sync);
    session.beginFrame({});

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; };

    const auto scene = await(session, window, session.createScene("Canvas2DLifecycle"));
    await(session, window, session.setActiveScene(scene.scene_id, true));

    // 1. register the Canvas2D factory → its register_ops_fn allocates the submit op.
    const auto canvas_type = await(session, window,
        session.registerFeatureType(kCanvas2DFeatureFactory));
    check(canvas_type.feature_type_id != 0u, "registerFeatureType(Canvas2D) succeeded (dynamic ops registered)");

    // 2. first Canvas2D attaches.
    const auto cf = await(session, window,
        session.addFeature(scene.scene_id, canvas_type.feature_type_id, EmptyConfig{}));
    check(cf.feature.valid(), "addFeature(Canvas2D) succeeded");

    // 3. a SECOND Canvas2D on the same scene is rejected (SinglePerScene).
    const auto cf2 = await(session, window,
        session.addFeature(scene.scene_id, canvas_type.feature_type_id, EmptyConfig{}));
    check(!cf2.feature.valid(), "second Canvas2D rejected — SinglePerScene");

    // 4. a probe attached AFTER Canvas2D observes: Canvas2DResources present, and NO
    //    3D mesh arena (a Canvas-only scene never allocates InstanceResources).
    const auto probe_type = await(session, window, session.registerFeatureType(kProbeFactory()));
    await(session, window,
        session.addFeature(scene.scene_id, probe_type.feature_type_id, EmptyConfig{}));
    flush(session, window);
    check(SceneProbeFeature::canvas_res_present.load(std::memory_order_relaxed) == 1,
          "Canvas2DResources present after Canvas2D attach");
    check(SceneProbeFeature::instance_res_absent.load(std::memory_order_relaxed) == 1,
          "no 3D mesh arena in a Canvas-only scene (InstanceResources absent)");

    // 5. removeFeature detaches cleanly; Canvas2D is then re-installable.
    session.removeFeature(scene.scene_id, cf.feature);
    flush(session, window);
    const auto cf3 = await(session, window,
        session.addFeature(scene.scene_id, canvas_type.feature_type_id, EmptyConfig{}));
    check(cf3.feature.valid(), "Canvas2D re-installable after removal (clean detach)");

    sync->requestStop();
    server_thread.join();

    std::cout << (fails ? "\nFAILED\n" : "\nPASSED\n");
    return fails ? 1 : 0;
}
