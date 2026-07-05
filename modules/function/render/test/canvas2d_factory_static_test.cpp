// ============================================================================
//  canvas2d_factory_static_test.cpp — R2-01 (device-free / cpu tier): the
//  Canvas2DFeature factory + descriptor + submit-op protocol are well-formed.
//
//  This is the CI-safe half of R2-01's evidence (the real attach/detach lifecycle
//  needs a device → canvas2d_feature_lifecycle_test, gpu tier). It pins the
//  type-level contract that drives install policy: SinglePerScene, no per-view
//  state, a wired dynamic-op registrar, and a trivially-copyable Blob submit payload.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>  // factory + op protocol
#include <lux/engine/render/comm/RenderProtocol.hpp>                            // FeatureFactory (full def to read fields)
#include <lux/engine/render/core/FeatureDescriptor.hpp>                         // FeatureMultiplicity

#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace lux::render;

// Compile-time protocol contract.
static_assert(std::is_trivially_copyable_v<Canvas2DSubmitPayload>,
              "submit payload must be a trivially-copyable POD (the sprites ride a Blob, not the payload)");
static_assert(Canvas2DSubmitOp::kind == EOpKind::Blob,
              "the submit op is Blob-kind (variable-length sprite list, no borrowed pointer)");

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== canvas2d_factory_static_test (R2-01) ===\n");

    const FeatureFactory& f = kCanvas2DFeatureFactory;

    // Factory is fully wired — create + dynamic op registration + its inverse (R2-01 step 3).
    check(f.create_fn          != nullptr, "factory create_fn is wired");
    check(f.register_ops_fn    != nullptr, "factory register_ops_fn is wired (dynamic op registration)");
    check(f.unregister_ops_fn  != nullptr, "factory unregister_ops_fn is wired (clean op teardown)");
    check(f.name != nullptr && std::strcmp(f.name, "Canvas2D") == 0, "factory name is \"Canvas2D\"");

    // Type-level descriptor — the policy the install path enforces.
    check(f.descriptor.valid(), "descriptor carries a valid stable type id");
    check(f.descriptor.name == "Canvas2D", "descriptor name is \"Canvas2D\"");
    check(f.descriptor.multiplicity == FeatureMultiplicity::SinglePerScene,
          "descriptor is SinglePerScene (a second Canvas per scene is rejected)");
    check(f.descriptor.creates_view_state == false,
          "descriptor allocates no per-view state (2D uses the scene camera, not per-view GPU state)");
    check(f.descriptor.contributes_graph == true,
          "R2-02: contributes_graph=true — addPasses declares the Canvas SceneColor pass");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
