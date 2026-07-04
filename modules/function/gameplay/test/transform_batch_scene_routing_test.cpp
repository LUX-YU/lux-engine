// ============================================================================
//  transform_batch_scene_routing_test.cpp — runtime (headless, GPU-free) coverage
//  of the G-04 per-entry scene routing in the transform batch, over a fake
//  in-process render server (HeadlessBridgeFixture).
//
//  G-04: each TransformWriteEntry carries its OWN scene_id, so transform writes for
//  instances in DIFFERENT scenes (e.g. the editor main scene + a preview scene)
//  interleaved on the command ring never cross — the server routes each entry by its
//  own scene, not by one SetActiveScene scene for the whole batch. The client half of
//  that fix is MeshStackProxy::updateTransform stamping e.scene_id from its argument;
//  this test asserts the recorded batch entries carry the distinct, correct scene_ids.
//  A regression that dropped/hardcoded the per-entry scene_id would make the two
//  entries share a scene and flip the "distinct scene_ids" assertion.
//
//  Tests MeshStackProxy directly (no InstanceBridge / no assets needed).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>

#include <cstdio>

namespace
{
    int g_failures = 0;

    void check(bool cond, const char* msg)
    {
        std::printf("[%s] %s\n", cond ? " ok " : "FAIL", msg);
        if (!cond) ++g_failures;
    }
}

int main()
{
    using namespace lux::render;

    lux::bridgetest::HeadlessBridgeFixture fix;
    fix.registerMeshStackOps();

    MeshStackProxy proxy(fix.session(), fix.meshStackOps());

    const RenderSceneId    sceneA{ 3u, 1u };
    const RenderSceneId    sceneB{ 7u, 2u };   // distinct index AND generation
    const RenderObjectHandle objA{ 10u, 1u };
    const RenderObjectHandle objB{ 20u, 1u };
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    fix.session().beginFrame({});
    proxy.updateTransform(sceneA, objA, m);   // one bulk-of-1 TransformBatch
    proxy.updateTransform(sceneB, objB, m);   // another, for a DIFFERENT scene
    fix.roundTrip();

    check(fix.recorder().count("TransformBatch") == 2, "two updateTransform calls emit two transform batches");
    if (fix.recorder().count("TransformBatch") == 2)
    {
        const auto e0 = fix.recorder().payload<TransformWriteEntry>("TransformBatch", 0);
        const auto e1 = fix.recorder().payload<TransformWriteEntry>("TransformBatch", 1);
        check(e0.scene_id == sceneA && e0.object == objA, "entry 0 carries scene A + object A");
        check(e1.scene_id == sceneB && e1.object == objB, "entry 1 carries scene B + object B");
        check(e0.scene_id != e1.scene_id,
              "batches from different scenes carry DISTINCT scene_ids (G-04; fails if scene_id is dropped/shared)");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
