// ============================================================================
//  shadow_slice_math_test.cpp
//
//  GPU-free unit test for ShadowSliceMath.hpp (batch 7 / M19) — the pure shadow
//  projection math extracted from ShadowMapFeature::buildSlicesForView.
//
//  Exercises makePerspectiveLightSlice, the construction SHARED by spot lights
//  and point-light cube faces (review M25): perspective depth mapping, look-at
//  orthonormality/handedness, bias scaling + clamp, clip-plane passthrough, and
//  atlas-tile metadata. All inputs are POD; no Vulkan device is required.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/features/shadow/ShadowSliceMath.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace lux::render;

static int g_fail = 0;
static void check(bool cond, const char* name)
{
    std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name);
    if (!cond) ++g_fail;
}
static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static ShadowTileAllocation makeTile(uint32_t layer, uint32_t res,
                                     float sx, float sy, float bx, float by)
{
    ShadowTileAllocation t;
    t.layer      = layer;
    t.resolution = res;
    t.uv_scale   = Eigen::Vector2f(sx, sy);
    t.uv_bias    = Eigen::Vector2f(bx, by);
    return t;
}

// NDC of a world point under a slice's light_vp (with perspective divide).
static Eigen::Vector3f ndc(const ShadowSliceGPU& s, const Eigen::Vector3f& world)
{
    Eigen::Vector4f clip = s.light_vp * Eigen::Vector4f(world.x(), world.y(), world.z(), 1.f);
    return Eigen::Vector3f(clip.x() / clip.w(), clip.y() / clip.w(), clip.z() / clip.w());
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== shadow_slice_math_test ===\n");

    const Eigen::Vector3f pos(2.f, 3.f, -4.f);
    const Eigen::Vector3f fwd     = Eigen::Vector3f(1.f, -1.f, 0.5f).normalized();
    const Eigen::Vector3f up_seed(0.f, 1.f, 0.f);
    const float near_z = 0.1f, far_z = 50.f;
    const float scale  = 1.f / std::tan(0.7f * 0.5f); // spot-like 1/tan(fov/2)
    const ShadowTileAllocation t = makeTile(/*layer=*/3, /*res=*/1024, 0.5f, 0.5f, 0.25f, 0.0f);

    const ShadowSliceGPU s =
        makePerspectiveLightSlice(pos, fwd, up_seed, near_z, far_z, scale, 0.01f, t);

    // ── Projection depth: near plane -> NDC z=0 (centred), far plane -> z=1 ─────
    {
        const Eigen::Vector3f n = ndc(s, pos + fwd * near_z);
        const Eigen::Vector3f f = ndc(s, pos + fwd * far_z);
        check(approx(n.x(), 0.f) && approx(n.y(), 0.f), "on-axis point projects to NDC centre (x=y=0)");
        check(approx(n.z(), 0.f), "near-plane point -> NDC z = 0");
        check(approx(f.z(), 1.f), "far-plane point -> NDC z = 1");
    }

    // ── Look-at handedness: offsets along right / up land where expected ───────
    {
        const Eigen::Vector3f right = fwd.cross(up_seed).normalized();
        const Eigen::Vector3f up    = right.cross(fwd).normalized();
        const Eigen::Vector3f mid   = pos + fwd * 1.0f;
        const Eigen::Vector3f pr = ndc(s, mid + right * 0.1f);
        const Eigen::Vector3f pu = ndc(s, mid + up    * 0.1f);
        check(pr.x() > 1e-3f && approx(pr.y(), 0.f), "step along right -> +x NDC, y unchanged");
        check(pu.y() < -1e-3f && approx(pu.x(), 0.f), "step along up -> -y NDC (proj(1,1) negated), x unchanged");
    }

    // ── Bias scaling + clamp + clip planes + perspective flag ─────────────────
    {
        check(approx(s.bias, 0.01f * 1024.0f), "bias = shadow_bias * 1024");
        check(approx(s.slope_bias, 0.01f * 800.0f), "slope_bias = shadow_bias * 800");
        check(approx(s.shadow_near, near_z) && approx(s.shadow_far, far_z), "clip planes copied through");
        check(s.depth_is_perspective == 1u, "depth_is_perspective = 1 (perspective slice)");

        const ShadowSliceGPU s0 =
            makePerspectiveLightSlice(pos, fwd, up_seed, near_z, far_z, scale, 0.0f, t);
        check(approx(s0.slope_bias, 1e-5f), "slope_bias clamps to 1e-5 at zero bias");
    }

    // ── Atlas tile metadata ───────────────────────────────────────────────────
    {
        check(approx(s.texel_size, 1.f / 1024.f), "texel_size = 1 / resolution");
        check(approx(s.atlas_uv_scale.x(), 0.5f) && approx(s.atlas_uv_bias.x(), 0.25f),
              "atlas uv scale/bias copied from tile");
        check(s.atlas_layer == 3u, "atlas_layer copied from tile");
        check(s.atlas_inner_uv_min.x() > s.atlas_uv_bias.x()
           && s.atlas_inner_uv_max.x() < s.atlas_uv_bias.x() + s.atlas_uv_scale.x(),
              "inner sampling rect is inset within the tile");
    }

    // ── Determinism / M25: spot and point cube-face paths share one function ──
    {
        const ShadowSliceGPU a = makePerspectiveLightSlice(pos, fwd, up_seed, near_z, far_z, scale, 0.01f, t);
        const ShadowSliceGPU b = makePerspectiveLightSlice(pos, fwd, up_seed, near_z, far_z, scale, 0.01f, t);
        const bool same_vp = (a.light_vp - b.light_vp).cwiseAbs().maxCoeff() < 1e-6f;
        check(same_vp && approx(a.bias, b.bias),
              "identical inputs -> identical slice (deterministic shared path)");
    }

    std::printf("=== shadow_slice_math_test %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
