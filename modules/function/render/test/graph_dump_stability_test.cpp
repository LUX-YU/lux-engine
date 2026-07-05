// ============================================================================
//  graph_dump_stability_test.cpp — R2-02 (gpu tier): the Canvas2D render graph is
//  STABLE under per-frame CONTENT change. Submitting sprites (which flow through the
//  submit op → Canvas2DResources → onFrameBegin upload, NOT through addPasses) must
//  NOT invalidate / recompile the graph.
//
//  Signal: a feature's addPasses() is called ONLY when the graph (re)compiles. A
//  test-only GraphProbeFeature tallies its addPasses calls. We warm up (let the graph
//  compile + stabilise), snapshot the tally, submit a batch of sprites via Canvas2DProxy,
//  render more frames, and assert the tally is UNCHANGED — i.e. sprite content caused
//  zero recompiles. Also proves the empty→non-empty draw-list transition is crash-free
//  (the pass no-ops at draw_count==0 and draws afterwards).
//
//  Self-checking: 0 = PASS / skip (no Vulkan), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/FeatureDescriptor.hpp>

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>   // factory, proxy, ids
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>    // SpriteDraw

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
    // Server-side witness: its addPasses fires once per graph (re)compile. Counting the
    // calls counts the recompiles.
    struct GraphProbeFeature : RenderFeature
    {
        static std::atomic<int> add_passes_calls;
        GraphProbeFeature() : RenderFeature(RenderFeature::Config{"GraphProbe"}) {}
        void addPasses(RGBuilder& /*builder*/) override
        {
            add_passes_calls.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::atomic<int> GraphProbeFeature::add_passes_calls{0};

    FeatureHandle probeCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<GraphProbeFeature>();
    }
    // contributes_graph=true → the graph builder invokes addPasses for it on every compile.
    constexpr FeatureDescriptor kProbeDesc{
        .type              = featureId("lux.test.canvas2d.graphprobe.v1"),
        .name              = "GraphProbe",
        .contributes_graph = true,
    };
    FeatureFactory kProbeFactory() { return makeSimpleFactory(&probeCreateFn, "GraphProbe", kProbeDesc); }

    struct EmptyConfig {};

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
        s.submitFrame(true);
        while (!req.isReady()) { w.pollEvents(); s.waitAndPumpReplies(); }
        T r = req.result();
        s.beginFrame({});
        return r;
    }

    void flush(RenderSession& s, lux::window::LuxWindow& w)
    {
        s.submitFrame(true);
        w.pollEvents();
        s.waitAndPumpReplies();
        s.beginFrame({});
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== graph_dump_stability_test (R2-02) ===\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(64, 64, "graph_dump_stability_test");

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r) { failed.store(true); ready.store(true); return; }
        if (auto r = server.attachToWindow(window); !r) { failed.store(true); ready.store(true); return; }
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

    const auto scene = await(session, window, session.createScene("Canvas2DGraph"));
    await(session, window, session.setActiveScene(scene.scene_id, true));
    await(session, window, session.addView(scene.scene_id, {64, 64}, "main"));

    // Install Canvas2D (the subject) + the graph probe (the witness).
    const auto canvas_reg = await(session, window, session.registerFeatureType(kCanvas2DFeatureFactory));
    await(session, window, session.addFeature(scene.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));
    const auto probe_reg = await(session, window, session.registerFeatureType(kProbeFactory()));
    await(session, window, session.addFeature(scene.scene_id, probe_reg.feature_type_id, EmptyConfig{}));

    // Warm up: let the graph compile + stabilise across a few frames (empty draw list —
    // the Canvas pass must no-op safely at draw_count==0).
    for (int i = 0; i < 4; ++i) flush(session, window);
    const int compiles_before = GraphProbeFeature::add_passes_calls.load(std::memory_order_relaxed);
    check(compiles_before >= 1, "graph compiled at least once during warm-up");

    // CONTENT change: submit a batch of sprites through the real submit op.
    Canvas2DOperationIds canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);
    check(canvas_ops.valid(), "Canvas2D op ids resolved from the registration reply");

    SpriteDraw a{};
    a.transform[0] = 2.f; a.transform[5] = 2.f; a.transform[10] = 1.f; a.transform[15] = 1.f; // scale 2, at origin
    a.uv = Rect2D{0.f, 0.f, 1.f, 1.f};
    a.tint = 0xFFFFFFFFu;
    a.key = DrawOrderKey{0, 0, 0, 0, 1};
    SpriteDraw b = a;
    b.transform[12] = 3.f; b.transform[13] = 1.f;   // translated
    b.tint = 0x80204080u;
    b.key = DrawOrderKey{0, 0, 0, 0, 2};
    const SpriteDraw batch[] = { a, b };

    Canvas2DProxy(session, canvas_ops).submitSprites(scene.scene_id, batch);

    // Render more frames: the sprites get ingested + drawn (non-empty draw list).
    for (int i = 0; i < 4; ++i) flush(session, window);
    const int compiles_after = GraphProbeFeature::add_passes_calls.load(std::memory_order_relaxed);

    check(compiles_after == compiles_before,
          "submitting sprites caused ZERO graph recompiles (content ≠ topology)");

    // A second, different batch — still no recompile.
    SpriteDraw c = a; c.tint = 0x40108020u; c.key = DrawOrderKey{1, 0, 0, 0, 3};
    const SpriteDraw batch2[] = { c };
    Canvas2DProxy(session, canvas_ops).submitSprites(scene.scene_id, batch2);
    for (int i = 0; i < 4; ++i) flush(session, window);
    check(GraphProbeFeature::add_passes_calls.load(std::memory_order_relaxed) == compiles_before,
          "a second, different sprite batch also caused zero recompiles");

    sync->requestStop();
    server_thread.join();

    std::cout << (fails ? "\nFAILED\n" : "\nPASSED\n");
    return fails ? 1 : 0;
}
