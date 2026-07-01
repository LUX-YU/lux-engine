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
//    3c    addFeature that CONFLICTS with an installed feature is rejected.
//    3c    a cyclic required dependency (A<->B) rejects both (hard-reject model).
//    PR-4  a STALE scene handle (used after destroyScene) is safely rejected,
//          not misrouted onto a recycled slot.
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
        static std::atomic<bool> desc_valid_at_attach;

        CountingFeature() : RenderFeature(RenderFeature::Config{"Counting"}) {}

        lux::render::Expected<void> initAndAttachTo(RenderScene& /*scene*/) override
        {
            // 三-2: the type-level descriptor must already be applied here (BEFORE
            // attach), not only afterwards — so descriptor() is valid during attach.
            desc_valid_at_attach.store(descriptor().valid(), std::memory_order_relaxed);
            return {};
        }

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
    std::atomic<bool> CountingFeature::desc_valid_at_attach{false};

    // Test-only feature whose allocateViewState FAILS once a configured count is
    // reached, to exercise the transactional rollback in addFeatureImpl (三-3).
    struct FailingFeature : RenderFeature
    {
        static std::atomic<int> alloc_ok;   // successful allocateViewState calls
        static std::atomic<int> dealloc;    // deallocateViewState calls (rollback)
        static std::atomic<int> fail_at;    // start failing once alloc_ok == fail_at

        FailingFeature() : RenderFeature(RenderFeature::Config{"Failing"}) {}

        bool allocateViewState(std::uint32_t /*view*/, RenderScene& /*scene*/) override
        {
            if (alloc_ok.load(std::memory_order_relaxed) >= fail_at.load(std::memory_order_relaxed))
                return false;   // simulate a GPU-resource allocation failure
            alloc_ok.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        void deallocateViewState(std::uint32_t /*view*/) override
        {
            dealloc.fetch_add(1, std::memory_order_relaxed);
        }
        void addPasses(RGBuilder& /*builder*/) override {}
    };
    std::atomic<int> FailingFeature::alloc_ok{0};
    std::atomic<int> FailingFeature::dealloc{0};
    std::atomic<int> FailingFeature::fail_at{0};

    FeatureHandle failingCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<FailingFeature>();
    }
    constexpr FeatureDescriptor kFailingDesc{
        .type               = featureId("lux.test.failing.v1"),
        .name               = "Failing",
        .creates_view_state = true,
    };
    FeatureFactory kFailingFactory() { return makeSimpleFactory(&failingCreateFn, "Failing", kFailingDesc); }

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

    // 三-4 test descriptors — the instance type is irrelevant (reuse countingCreateFn);
    // only the descriptor (type / deps / multiplicity) drives the policy under test.
    constexpr FeatureDescriptor kProviderDesc{
        .type = featureId("lux.test.provider.v1"), .name = "Provider",
    };
    constexpr FeatureDependency kConsumerDeps[] = { { featureId("lux.test.provider.v1"), /*optional=*/false } };
    constexpr FeatureDescriptor kConsumerDesc{
        .type = featureId("lux.test.consumer.v1"), .name = "Consumer", .dependencies = kConsumerDeps,
    };
    constexpr FeatureDescriptor kSingleDesc{
        .type = featureId("lux.test.single.v1"), .name = "Single",
        .multiplicity = FeatureMultiplicity::SinglePerScene,
    };
    FeatureFactory kProviderFactory() { return makeSimpleFactory(&countingCreateFn, "Provider", kProviderDesc); }
    FeatureFactory kConsumerFactory() { return makeSimpleFactory(&countingCreateFn, "Consumer", kConsumerDesc); }
    FeatureFactory kSingleFactory()   { return makeSimpleFactory(&countingCreateFn, "Single",   kSingleDesc);   }

    // Test-only feature whose attach FAILS — exercises the install-abort path (三-3).
    struct FailAttachFeature : RenderFeature
    {
        FailAttachFeature() : RenderFeature(RenderFeature::Config{"FailAttach"}) {}
        lux::render::Expected<void> initAndAttachTo(RenderScene& /*scene*/) override
        {
            return lux::cxx::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        void addPasses(RGBuilder& /*builder*/) override {}
    };
    FeatureHandle failAttachCreateFn(void* scene, const void*, std::size_t)
    {
        return static_cast<RenderScene*>(scene)->addFeature<FailAttachFeature>();
    }
    constexpr FeatureDescriptor kFailAttachDesc{
        .type = featureId("lux.test.failattach.v1"), .name = "FailAttach",
    };
    FeatureFactory kFailAttachFactory() { return makeSimpleFactory(&failAttachCreateFn, "FailAttach", kFailAttachDesc); }

    // ── conflict test: Conflict declares a CONFLICT with Base; installing Base
    // then Conflict must reject Conflict (descriptor.conflicts). ────────────────
    constexpr FeatureDescriptor kBaseDesc{
        .type = featureId("lux.test.base.v1"), .name = "Base",
    };
    constexpr FeatureTypeId kConflictConflicts[] = { featureId("lux.test.base.v1") };
    constexpr FeatureDescriptor kConflictDesc{
        .type = featureId("lux.test.conflict.v1"), .name = "Conflict",
        .conflicts = kConflictConflicts,
    };
    FeatureFactory kBaseFactory()     { return makeSimpleFactory(&countingCreateFn, "Base",     kBaseDesc); }
    FeatureFactory kConflictFactory() { return makeSimpleFactory(&countingCreateFn, "Conflict", kConflictDesc); }

    // ── cyclic-dependency test: A requires B and B requires A. Under hard-reject
    // (no auto-install) neither can be the first installed → both rejected. ──────
    constexpr FeatureDependency kCycleADeps[] = { { featureId("lux.test.cycleB.v1"), /*optional=*/false } };
    constexpr FeatureDependency kCycleBDeps[] = { { featureId("lux.test.cycleA.v1"), /*optional=*/false } };
    constexpr FeatureDescriptor kCycleADesc{
        .type = featureId("lux.test.cycleA.v1"), .name = "CycleA", .dependencies = kCycleADeps,
    };
    constexpr FeatureDescriptor kCycleBDesc{
        .type = featureId("lux.test.cycleB.v1"), .name = "CycleB", .dependencies = kCycleBDeps,
    };
    FeatureFactory kCycleAFactory() { return makeSimpleFactory(&countingCreateFn, "CycleA", kCycleADesc); }
    FeatureFactory kCycleBFactory() { return makeSimpleFactory(&countingCreateFn, "CycleB", kCycleBDesc); }

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
    check(CountingFeature::desc_valid_at_attach.load(),
          "feature observes its own descriptor DURING attach (三-2)");
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

    // Repeat removeView on the SAME (now Destroying) view → idempotent no-op: no
    // second release, no re-notify, no duplicate pending-destroy (五-4). (No crash
    // through the following frames proves the duplicate record wasn't enqueued.)
    session.removeView(scene.scene_id, view_a.view);
    flush(session, window);
    check(CountingFeature::dealloc_count.load() - dealloc_before == 1,
          "repeat removeView on a Destroying view is a no-op (五-4)");

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

    // ── P0-2: destroying a WHOLE scene releases ALL feature per-view state ──
    // A fresh scene with 2 views + a creates_view_state feature, then destroyScene
    // directly (NO removeView / removeFeature). shutdownFull() must run
    // deallocateViewState() for every (feature,view) pair — the bare onDetach loop
    // it used before skipped them entirely on whole-scene teardown.
    {
        const auto sc2 = await(session, window, session.createScene("Shutdown"));
        await(session, window, session.setActiveScene(sc2.scene_id, true));
        await(session, window, session.addView(sc2.scene_id, {64, 64}, "SA"));
        await(session, window, session.addView(sc2.scene_id, {64, 64}, "SB"));
        const auto cf2 = await(session, window,
            session.addFeature(sc2.scene_id, counting_type.feature_type_id, EmptyConfig{}));
        check(cf2.feature.valid(), "addFeature(Counting) into a 2-view scene");
        flush(session, window);

        const int dealloc_before_destroy = CountingFeature::dealloc_count.load();
        session.destroyScene(sc2.scene_id);   // direct teardown — not removeView/removeFeature
        // removeScene retires the scene ASYNChronously (PR-6): shutdownFull() runs a
        // few frames later via collectRetiredScenes once the GPU passes the scene's
        // last serial. Pump frames until it fires (bounded).
        for (int i = 0; i < 8 &&
             (CountingFeature::dealloc_count.load() - dealloc_before_destroy) < 2; ++i)
            flush(session, window);
        check(CountingFeature::dealloc_count.load() - dealloc_before_destroy == 2,
              "destroyScene releases per-view state for all views (P0-2 shutdownFull)");
    }

    // ── 三-3: a feature whose Nth view-state allocation FAILS is rolled back and
    // the install rejected (strong failure guarantee — no half-attached feature,
    // no leaked per-view state). 2 views, fail on the 2nd allocation.
    {
        const auto sc3 = await(session, window, session.createScene("Rollback"));
        await(session, window, session.setActiveScene(sc3.scene_id, true));
        await(session, window, session.addView(sc3.scene_id, {64, 64}, "RA"));
        await(session, window, session.addView(sc3.scene_id, {64, 64}, "RB"));
        const auto fail_type = await(session, window,
            session.registerFeatureType(kFailingFactory()));
        FailingFeature::alloc_ok.store(0);
        FailingFeature::dealloc.store(0);
        FailingFeature::fail_at.store(1);   // 1st view ok, 2nd view fails
        const auto ff = await(session, window,
            session.addFeature(sc3.scene_id, fail_type.feature_type_id, EmptyConfig{}));
        flush(session, window);
        check(!ff.feature.valid(),
              "addFeature rejected when a view-state allocation fails (三-3)");
        check(FailingFeature::dealloc.load() == FailingFeature::alloc_ok.load() &&
              FailingFeature::alloc_ok.load() == 1,
              "partial per-view state rolled back on failed install — no leak (三-3)");
    }

    // ── 三-4: SinglePerScene multiplicity + reverse-dependency removal guard ──
    {
        const auto sc4 = await(session, window, session.createScene("Multiplicity"));
        await(session, window, session.setActiveScene(sc4.scene_id, true));

        // SinglePerScene: first install succeeds, second of the same type is rejected.
        const auto single_type = await(session, window, session.registerFeatureType(kSingleFactory()));
        const auto s1 = await(session, window,
            session.addFeature(sc4.scene_id, single_type.feature_type_id, EmptyConfig{}));
        const auto s2 = await(session, window,
            session.addFeature(sc4.scene_id, single_type.feature_type_id, EmptyConfig{}));
        check(s1.feature.valid() && !s2.feature.valid(),
              "SinglePerScene rejects a second instance (三-4)");

        // Reverse-dependency: Consumer requires Provider.
        const auto prov_type = await(session, window, session.registerFeatureType(kProviderFactory()));
        const auto cons_type = await(session, window, session.registerFeatureType(kConsumerFactory()));
        const auto prov = await(session, window,
            session.addFeature(sc4.scene_id, prov_type.feature_type_id, EmptyConfig{}));
        const auto cons = await(session, window,
            session.addFeature(sc4.scene_id, cons_type.feature_type_id, EmptyConfig{}));
        check(prov.feature.valid() && cons.feature.valid(), "Provider + Consumer installed");

        // Removing Provider while Consumer depends on it is REJECTED → Provider stays,
        // so a second Consumer (whose required dep is present) still installs.
        session.removeFeature(sc4.scene_id, prov.feature);
        flush(session, window);
        const auto cons2 = await(session, window,
            session.addFeature(sc4.scene_id, cons_type.feature_type_id, EmptyConfig{}));
        check(cons2.feature.valid(),
              "removeFeature rejected while a dependent exists — provider stays (三-4)");

        // Remove the dependents first, THEN Provider removes cleanly → a later Consumer
        // is rejected because its required dependency is now gone.
        session.removeFeature(sc4.scene_id, cons.feature);
        session.removeFeature(sc4.scene_id, cons2.feature);
        flush(session, window);
        session.removeFeature(sc4.scene_id, prov.feature);
        flush(session, window);
        const auto cons3 = await(session, window,
            session.addFeature(sc4.scene_id, cons_type.feature_type_id, EmptyConfig{}));
        check(!cons3.feature.valid(),
              "provider removable once no dependents remain — later dependent rejected (三-4)");
    }

    // ── 三-3 (attach): a feature whose initAndAttachTo FAILS aborts the install —
    // invalid handle returned, no half-attached feature left in the scene.
    {
        const auto fa_type = await(session, window, session.registerFeatureType(kFailAttachFactory()));
        const auto fa = await(session, window,
            session.addFeature(scene.scene_id, fa_type.feature_type_id, EmptyConfig{}));
        check(!fa.feature.valid(),
              "feature whose attach fails is rejected — install aborted (三-3 attach)");
    }

    // ── 3c (conflict): installing a feature that declares a CONFLICT with an
    // already-installed feature is rejected (Base installed → Conflict rejected).
    {
        const auto sc5 = await(session, window, session.createScene("Conflict"));
        await(session, window, session.setActiveScene(sc5.scene_id, true));
        const auto base_t = await(session, window, session.registerFeatureType(kBaseFactory()));
        const auto conf_t = await(session, window, session.registerFeatureType(kConflictFactory()));
        const auto base = await(session, window,
            session.addFeature(sc5.scene_id, base_t.feature_type_id, EmptyConfig{}));
        const auto conf = await(session, window,
            session.addFeature(sc5.scene_id, conf_t.feature_type_id, EmptyConfig{}));
        check(base.feature.valid() && !conf.feature.valid(),
              "addFeature rejected when it conflicts with an installed feature (3c conflict)");
    }

    // ── 3c (cycle): A requires B and B requires A. With hard-reject + no
    // auto-install, whichever is added first has its required dep unmet → BOTH
    // are rejected (there is no valid install order for a required cycle).
    {
        const auto sc6 = await(session, window, session.createScene("Cycle"));
        await(session, window, session.setActiveScene(sc6.scene_id, true));
        const auto a_t = await(session, window, session.registerFeatureType(kCycleAFactory()));
        const auto b_t = await(session, window, session.registerFeatureType(kCycleBFactory()));
        const auto a = await(session, window,
            session.addFeature(sc6.scene_id, a_t.feature_type_id, EmptyConfig{}));
        const auto b = await(session, window,
            session.addFeature(sc6.scene_id, b_t.feature_type_id, EmptyConfig{}));
        check(!a.feature.valid() && !b.feature.valid(),
              "cyclic required dependency (A<->B) rejects both (3c hard-reject)");
    }

    // ── PR-4: a STALE scene handle (used after destroyScene) is safely rejected.
    // destroyScene releases the scene's SlotKey slot immediately (PR-6), so the
    // old scene_id's generation no longer matches — the server refuses the op
    // (addView returns an invalid view) instead of hitting a recycled slot.
    {
        const auto sc7 = await(session, window, session.createScene("Stale"));
        await(session, window, session.setActiveScene(sc7.scene_id, true));
        session.destroyScene(sc7.scene_id);
        for (int i = 0; i < 4; ++i) flush(session, window);   // let the destroy settle
        const auto stale = await(session, window,
            session.addView(sc7.scene_id, {64, 64}, "StaleView"));
        check(!stale.view.valid(),
              "op on a stale scene handle (post-destroy) is rejected, not misrouted (PR-4)");
    }

    // ── P0-4: a reply-expecting command that FAILS to dispatch settles the client
    // request as FAILED (instead of hanging forever on a reply that never comes).
    // Send a command with an UNREGISTERED type_id → server's dispatcher emits a
    // CommandFailedReply, routed back to this request by request_id.
    {
        auto [failreq, failcb] = RenderRequestFactory<GenericOkReply>::make();
        SetActiveScenePayload dummy{};
        session.builder().pushWithReply<SetActiveScenePayload>(
            opcodes::CommandOp, TypeId{0x0000FFFEu}, dummy, std::move(failcb));
        for (int i = 0; i < 8 && !failreq.isReady(); ++i) flush(session, window);
        check(failreq.isReady() && failreq.failed(),
              "reply-expecting command that fails dispatch settles as failed, not hung (P0-4)");
    }

    sync->requestStop();
    server_thread.join();

    std::cout << "=== feature_view_lifecycle_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
