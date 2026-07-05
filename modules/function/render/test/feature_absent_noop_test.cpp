// ============================================================================
//  feature_absent_noop_test.cpp — R2-04 (cpu): a scene whose FeatureRegistry has NO
//  Canvas2DFeature (a pure-3D scene) yields INVALID Canvas2D op-ids. A Canvas2DProxy
//  built from invalid ops no-ops (its submitSprites early-returns on !ops.valid()), so
//  a 3D-only path pays nothing for 2D — the host simply never registers Canvas2D.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/comm/server/FeatureRegistry.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/comm/RenderCommTypes.hpp>   // makeTypeId

#include <cstdio>

using namespace lux::render;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== feature_absent_noop_test (R2-04) ===\n");

    FeatureRegistry reg;

    // A bare registry — nothing installed.
    check(!reg.has("Canvas2D"), "an empty registry has no Canvas2D");
    check(!reg.ops<Canvas2DOperationIds>("Canvas2D").valid(),
          "absent Canvas2D → invalid op-ids (the 2D bridge's proxy then no-ops)");

    // Even with OTHER features present (a real 3D scene), Canvas2D stays absent → the
    // lookup is name-specific, so 3D features never accidentally satisfy the 2D bridge.
    const TypeId light_ops[] = { makeTypeId(0, 1) };
    reg.injectForTest("Light", light_ops);
    reg.injectForTest("StandardMeshStack", light_ops);
    check(reg.has("Light"), "the 3D features ARE present");
    check(!reg.ops<Canvas2DOperationIds>("Canvas2D").valid(),
          "a 3D scene with Light/MeshStack still has NO Canvas2D ops");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
