// ============================================================================
//  feature_view_lifecycle_test.cpp
//
//  Asserts the feature / view lifecycle behaviours added across the render
//  refactor (PR-1 + 阶段 3), via a test-only CountingFeature whose
//  allocate/deallocateViewState are tallied in static atomics:
//
//    PR-1  addFeature into a scene with N views back-fills N per-view states.
//    PR-1  addView after the feature allocates one more.
//    PR-1  removeView releases exactly one.
//    PR-1  removeFeature releases the feature's remaining per-view states
//          (the leak that removeView-only used to miss).
//    3b    setFeatureEnabled(false/true) releases / re-allocates per-view state
//          (gated by descriptor.creates_view_state).
//    3c    addFeature with an unmet REQUIRED dependency is hard-rejected
//          (invalid handle), feature not created.
//
//  Reuses readback_test's window-attached server+session harness (proven). The
//  CountingFeature contributes no graph passes, so no rendering is needed — the
//  lifecycle ops run during command dispatch. Self-checking: 0 = PASS / skip
//  (no Vulkan), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>     // RenderScene::addFeature<T> in create_fn
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/FeatureDescriptor.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace lux::render;

// ── Test-only feature: tallies per-view-state alloc/dealloc ─────────────────
namespace
{
    struct CountingFeature : RenderFeature
    {
        static std::atomic<int> alloc_count;
        static std::atomic<int> dealloc_count;

        CountingFeature() : RenderFeature(RenderFeature::Config{"Counting"}) {}

        bool allocateViewState(std::uint32_t /*view*/, RenderScene& /*scene*/) override
        {
            alloc_count.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        void deallocateViewState(std::uint32_t /*view*/) override
        {
            dealloc_count.fetch_add(1, std::memory_order_relaxed);
        }
        void addPasses(RGBuilder& /*builder*/) override {}
    };
    std::atomic<int> CountingFeature::alloc_count{0};
    std::atomic<int> CountingFeature::dealloc_count{0};

    FeatureHandle countingCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<CountingFeature>();
    }

    // creates_view_state=true → setFeatureEnabled manages its per-view state (3b).
    constexpr FeatureDescriptor kCountingDesc{
        .type               = featureId("lux.test.counting.v1"),
        .name               = "Counting",
        .creates_view_state = true,
    };

    // A feature that REQUIRES a type that is never registered → must be rejected (3c).
    constexpr FeatureDependency kDependentDeps[] = {
        { featureId("lux.test.absent.v1"), /*optional=*/false },
    };
    constexpr FeatureDescriptor kDependentDesc{
        .type         = featureId("lux.test.dependent.v1"),
        .name         = "Dependent",
        .dependencies = kDependentDeps,
    };

    // Trivially-copyable empty config (these test features take no params).
    struct EmptyConfig {};

    FeatureFactory kCountingFactory()  { return makeSimpleFactory(&countingCreateFn, "Counting",  kCountingDesc); }
    // Dependent reuses countingCreateFn — never invoked (rejected before create).
    FeatureFactory kDependentFactory() { return makeSimpleFactory(&countingCreateFn, "Dependent", kDependentDesc); }

    std::vector<const char*> getVulkanExtensions()
    {
        glfwInit();
        std::uint32_t count = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&count);
        std::vector<const char*> result;
        for (std::uint32_t i = 0; i < count; ++i) result.emplace_back(exts[i]);
        return result;
    }

    // Submit blocking + pump replies until @p req resolves (readback_test pattern).
    template <class T>
    T await(RenderSession& s, lux::window::LuxWindow& w, RenderRequest<T> req)
    {
        s.submitFrame(/*blocking=*/true);
        while (!req.isReady()) { w.pollEvents(); s.waitAndPumpReplies(); }
        T r = req.result();
        s.beginFrame({});
        return r;
    }

    // Flush queued fire-and-forget ops (removeView / removeFeature / setFeatureEnabled)
    // so the server has processed them before we read the counters.
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
    std::cout << "=== feature_view_lifecycle_test ===\n";

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(64, 64, "feature_view_lifecycle_test");

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

    const auto scene = await(session, window, session.createScene("Lifecycle"));
    await(session, window, session.setActiveScene(scene.scene_id, true));

    // Register the counting feature type.
    const auto counting_type = await(session, window,
        session.registerFeatureType(kCountingFactory()));

    // view A, then add the feature → back-fill 1 (PR-1).
    const auto view_a = await(session, window, session.addView(scene.scene_id, {64, 64}, "A"));
    const int alloc_before = CountingFeature::alloc_count.load();
    const auto cf = await(session, window,
        session.addFeature(scene.scene_id, counting_type.feature_type_id, EmptyConfig{}));
    check(cf.feature.valid(), "addFeature(Counting) succeeded");
    flush(session, window);
    check(CountingFeature::alloc_count.load() - alloc_before == 1,
          "addFeature back-fills per-view state for the 1 existing view (PR-1)");

    // view B → +1 alloc.
    const auto view_b = await(session, window, session.addView(scene.scene_id, {64, 64}, "B"));
    flush(session, window);
    check(CountingFeature::alloc_count.load() - alloc_before == 2,
          "addView after feature allocates one more (PR-1)");

    // removeView(A) → +1 dealloc.
    int dealloc_before = CountingFeature::dealloc_count.load();
    session.removeView(scene.scene_id, view_a.view);
    flush(session, window);
    check(CountingFeature::dealloc_count.load() - dealloc_before == 1,
          "removeView releases exactly one per-view state (PR-1)");

    // disable → release remaining (B); enable → re-allocate (3b, creates_view_state).
    dealloc_before = CountingFeature::dealloc_count.load();
    session.setFeatureEnabled(scene.scene_id, cf.feature, false);
    flush(session, window);
    check(CountingFeature::dealloc_count.load() - dealloc_before == 1,
          "setFeatureEnabled(false) releases the remaining per-view state (3b)");

    int alloc_before2 = CountingFeature::alloc_count.load();
    session.setFeatureEnabled(scene.scene_id, cf.feature, true);
    flush(session, window);
    check(CountingFeature::alloc_count.load() - alloc_before2 == 1,
          "setFeatureEnabled(true) re-allocates the per-view state (3b)");

    // removeFeature → release its remaining per-view state (PR-1 leak fix).
    dealloc_before = CountingFeature::dealloc_count.load();
    session.removeFeature(scene.scene_id, cf.feature);
    flush(session, window);
    check(CountingFeature::dealloc_count.load() - dealloc_before == 1,
          "removeFeature releases the feature's remaining per-view state (PR-1)");

    // 3c: a feature with an unmet REQUIRED dependency is hard-rejected.
    const auto dep_type = await(session, window,
        session.registerFeatureType(kDependentFactory()));
    const auto dep = await(session, window,
        session.addFeature(scene.scene_id, dep_type.feature_type_id, EmptyConfig{}));
    check(!dep.feature.valid(),
          "addFeature with unmet required dependency is rejected (3c)");

    sync->requestStop();
    server_thread.join();

    std::cout << "=== feature_view_lifecycle_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
