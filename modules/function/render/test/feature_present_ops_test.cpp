// ============================================================================
//  feature_present_ops_test.cpp — R2-04 (cpu): a 2D scene whose FeatureRegistry HAS a
//  Canvas2DFeature yields VALID Canvas2D op-ids, addressed BY NAME ("Canvas2D") exactly
//  like every other feature — so a bespoke 2D bridge acquires working ops without any
//  bespoke-2D entry in the core ERenderableKind enum.
//
//  (The full round-trip — real registration → valid ops → successful submit → draw — is
//  covered end-to-end on a device by graph_dump_stability_test; this pins the name-keyed
//  ops-availability contract headlessly.)
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
    std::printf("=== feature_present_ops_test (R2-04) ===\n");

    FeatureRegistry reg;

    // Simulate the host having registered Canvas2D (its factory allocated one dynamic
    // op id — the submit op). Canvas2DOperationIds has exactly one op (Canvas2DSubmitOp).
    const TypeId canvas_ops[] = { makeTypeId(3, 1) };
    reg.injectForTest("Canvas2D", canvas_ops);

    check(reg.has("Canvas2D"), "the registry reports Canvas2D present");

    const auto ops = reg.ops<Canvas2DOperationIds>("Canvas2D");
    check(ops.valid(), "present Canvas2D → VALID op-ids resolved by name");
    check(ops.id<Canvas2DSubmitOp>() == canvas_ops[0],
          "the submit op id round-trips through FeatureOpIds::fromOps");

    // Name-keyed: a different name does NOT resolve to Canvas2D's ops.
    check(!reg.ops<Canvas2DOperationIds>("NotCanvas2D").valid(),
          "the lookup is name-specific (a wrong name yields invalid ops)");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
