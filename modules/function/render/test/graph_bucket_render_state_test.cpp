// ============================================================================
//  graph_bucket_render_state_test.cpp
//
//  Device-free unit test for the W3a render-side core: VariantBucketManager
//  must give graph materials DISTINCT variant buckets (= distinct PSOs) when
//  their render-state (alpha_mode / double_sided) differs, even when they share
//  the same baked shaders (same shader_key) — and must reuse ONE bucket for
//  identical (shader_key, render-state). It also checks the bucket descriptor
//  carries the render-state so the PSO build + BucketPipelinePool::pick() route
//  correctly (double_sided -> feature_mask DOUBLE_SIDED -> the no-cull tier).
//
//  Self-checking: 0 = PASS, 1 = FAIL. No Vulkan device needed.
// ============================================================================

#include <lux/engine/render/resources/material/VariantBucketManager.hpp>

#include <cstdint>
#include <cstdio>

using namespace lux::render;
namespace rdesc = lux::rdesc;

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== graph_bucket_render_state_test (W3a) ===\n");

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name); if (!cond) ++fails; };

    VariantBucketManager m;
    m.seedFamilyBootstrapBuckets();

    const ShaderHandle gb{ 1u, 1u };
    const ShaderHandle fw{ 2u, 1u };
    const std::uint64_t key = 0xA5A5'1234'DEAD'BEEFull;  // one baked-shader fingerprint

    // Same key + same render-state -> the SAME bucket (shared PSO).
    const uint32_t opaque  = m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Opaque, false);
    const uint32_t opaque2 = m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Opaque, false);
    check(opaque == opaque2, "identical (shader_key, render-state) -> SAME bucket");

    // Same key but different render-state -> DISTINCT buckets (distinct PSOs).
    const uint32_t dsided = m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Opaque, true);
    const uint32_t masked = m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Mask,   false);
    const uint32_t blended= m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Blend,  false);
    check(dsided  != opaque, "double_sided=true -> DISTINCT bucket from single-sided");
    check(masked  != opaque && masked != dsided, "alpha_mode=Mask -> DISTINCT bucket");
    check(blended != opaque && blended != masked, "alpha_mode=Blend -> DISTINCT bucket");

    // ... and re-querying each combo is stable (returns its own bucket again).
    check(m.getOrCreateGraph(key, gb, fw, rdesc::EAlphaMode::Opaque, true) == dsided,
          "re-query double_sided combo -> same bucket (stable key)");

    // The descriptor carries render-state for the PSO build, and double_sided sets
    // feature_mask DOUBLE_SIDED so BucketPipelinePool::pick() picks the no-cull tier.
    const auto d_op = m.at(opaque);
    check(d_op.graph_double_sided == false
          && !hasFeature(d_op.feature_mask, EShaderFeature::DOUBLE_SIDED),
          "single-sided bucket: graph_double_sided=false + no DOUBLE_SIDED feature");

    const auto d_ds = m.at(dsided);
    check(d_ds.graph_double_sided == true
          && hasFeature(d_ds.feature_mask, EShaderFeature::DOUBLE_SIDED),
          "double-sided bucket: graph_double_sided=true + DOUBLE_SIDED feature (-> no-cull tier)");

    const auto d_mask = m.at(masked);
    check(d_mask.graph_alpha_mode == rdesc::EAlphaMode::Mask, "mask bucket carries graph_alpha_mode=Mask");

    const auto d_blend = m.at(blended);
    check(d_blend.graph_alpha_mode == rdesc::EAlphaMode::Blend, "blend bucket carries graph_alpha_mode=Blend");

    // A DIFFERENT shader fingerprint with the SAME render-state is still distinct.
    const ShaderHandle gb2{ 3u, 1u };
    const std::uint64_t key2 = 0x1111'2222'3333'4444ull;
    const uint32_t other = m.getOrCreateGraph(key2, gb2, fw, rdesc::EAlphaMode::Opaque, false);
    check(other != opaque, "different shader_key -> distinct bucket (fingerprint preserved)");

    std::printf("=== graph_bucket_render_state_test %s (fails=%d) ===\n",
                fails == 0 ? "PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
