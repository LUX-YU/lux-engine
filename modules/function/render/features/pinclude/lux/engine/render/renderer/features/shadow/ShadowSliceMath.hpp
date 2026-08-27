#pragma once
/**
 * @file ShadowSliceMath.hpp
 * @brief Pure (GPU-free) shadow-slice projection helpers, split out of
 *        ShadowMapFeature::buildSlicesForView so the math is unit-testable.
 *
 * Everything here operates on plain POD inputs (positions, directions, clip
 * planes, an atlas tile) and produces a POD ShadowSliceGPU — no Vulkan, no
 * device (see shadow_slice_math_test).
 *
 * The atlas PACKER lives here too, together with the rule that decides where a
 * caster lands. That boundary moved deliberately: packing used to be filed under
 * "orchestration" and left inside ShadowMapFeature.cpp, until a flicker bug
 * showed that placement carries a real invariant —
 *
 *     a caster's atlas tile is a function of its identity and of which casters
 *     are present, NEVER of the order they were scored in
 *
 * — and an invariant that nothing can test is an invariant that comes back.
 * Light SELECTION (scoring, the cascade budget, per-light frusta) stays in
 * ShadowMapFeature.cpp; only the packing and the placement order are here.
 */

#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp> // ShadowSliceGPU (+ Eigen/Core)

#include <Eigen/Geometry> // Eigen::Vector3f::cross

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace lux::render
{
    /// One packed shadow tile in the atlas (page layer + UV rect + resolution).
    struct ShadowTileAllocation
    {
        uint32_t layer{0};
        uint32_t x{0};
        uint32_t y{0};
        uint32_t resolution{1};
        Eigen::Vector2f uv_scale{1.0f, 1.0f};
        Eigen::Vector2f uv_bias{0.0f, 0.0f};
    };

    /// Fill a slice's atlas-sampling metadata (texel size, UV scale/bias, the
    /// one-texel-inset inner sampling rect, and the array layer) from a tile.
    inline void assignAtlasTileMetadata(ShadowSliceGPU& slice, const ShadowTileAllocation& tile)
    {
        const float resolution = static_cast<float>(std::max(tile.resolution, 1u));

        slice.texel_size = 1.0f / resolution;
        slice.atlas_uv_scale = tile.uv_scale;
        slice.atlas_uv_bias = tile.uv_bias;

        // Keep a one-texel inset so +/-1 PCF taps stay inside the tile interior.
        const Eigen::Vector2f guard = tile.uv_scale * slice.texel_size;
        Eigen::Vector2f inner_min = tile.uv_bias + guard;
        Eigen::Vector2f inner_max = tile.uv_bias + tile.uv_scale - guard;
        const Eigen::Vector2f center = tile.uv_bias + tile.uv_scale * 0.5f;

        if (inner_min.x() > inner_max.x())
        {
            inner_min.x() = center.x();
            inner_max.x() = center.x();
        }
        if (inner_min.y() > inner_max.y())
        {
            inner_min.y() = center.y();
            inner_max.y() = center.y();
        }

        slice.atlas_inner_uv_min = inner_min;
        slice.atlas_inner_uv_max = inner_max;
        slice.atlas_layer = tile.layer;
    }

    /// Build a perspective shadow slice — a spot light, or one point-light cube
    /// face. This is the construction shared by both paths (review M25).
    ///
    /// @param pos            Light world position (projection center).
    /// @param fwd            Unit forward (look) direction.
    /// @param up_seed        Seed up vector; re-orthonormalized against @p fwd.
    /// @param near_z,far_z   Light projection clip planes.
    /// @param proj_xy_scale  Reciprocal half-FOV tangent: 1/tan(fov/2) for a spot,
    ///                       1/overlap for a (slightly widened 90°) cube face.
    /// @param shadow_bias    The light's raw shadow bias (scaled into bias/slope_bias).
    /// @param tile           The packed atlas tile this slice renders into.
    inline ShadowSliceGPU makePerspectiveLightSlice(
        const Eigen::Vector3f& pos,
        const Eigen::Vector3f& fwd,
        const Eigen::Vector3f& up_seed,
        float near_z,
        float far_z,
        float proj_xy_scale,
        float shadow_bias,
        const ShadowTileAllocation& tile
    )
    {
        Eigen::Matrix4f proj = Eigen::Matrix4f::Zero();
        proj(0, 0) = proj_xy_scale;
        proj(1, 1) = -proj_xy_scale;
        proj(2, 2) = far_z / (far_z - near_z);
        proj(2, 3) = -(far_z * near_z) / (far_z - near_z);
        proj(3, 2) = 1.f;

        const Eigen::Vector3f right = fwd.cross(up_seed).normalized();
        const Eigen::Vector3f up = right.cross(fwd).normalized();

        Eigen::Matrix4f view_m = Eigen::Matrix4f::Identity();
        view_m.block<1, 3>(0, 0) = right.transpose();
        view_m.block<1, 3>(1, 0) = up.transpose();
        view_m.block<1, 3>(2, 0) = fwd.transpose();
        view_m(0, 3) = -right.dot(pos);
        view_m(1, 3) = -up.dot(pos);
        view_m(2, 3) = -fwd.dot(pos);

        ShadowSliceGPU slice{};
        slice.light_vp = proj * view_m;
        // Point / spot lights use 4x the constant bias scale of directional — a
        // receiver perpendicular to the light (e.g. the floor directly under a
        // downward-facing light) collapses slope_bias to ~0, leaving only the
        // constant term, so they need the extra headroom to avoid acne.
        slice.bias = shadow_bias * 1024.0f;
        slice.slope_bias = std::max(shadow_bias * 800.0f, 1e-5f);
        // 透视切片(下面 depth_is_perspective=1)上**不使用**该字段:着色器的
        // 透视分支用 `2·texel_size·|clip.w|` 现算世界 texel 尺寸
        //(shadow_pcf.glsl 的 world_texel 三元式)。**它不是废弃字段** ——
        // 正交分支(方向光 CSM)照样读它,由 ShadowMapFeature 写入真实的世界
        // texel 尺寸(extent/resolution)。删了会让 CSM 的法线偏移归零 → 阴影瑕疵。
        slice.normal_bias = 0.0f;
        // Perspective slice: EVSM must linearize the hyperbolic NDC z via these
        // clip planes before warping (see ShadowSliceGPU comment).
        slice.shadow_near = near_z;
        slice.shadow_far = far_z;
        slice.depth_is_perspective = 1u;
        assignAtlasTileMetadata(slice, tile);
        return slice;
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Atlas packing
    // ════════════════════════════════════════════════════════════════════════

    /// Smallest tile the fallback ladder will shrink to before giving up.
    inline constexpr uint32_t kMinShadowTileResolution = 256u;

    [[nodiscard]] inline uint32_t roundUpPow2(uint32_t v) noexcept
    {
        if (v <= 1u)
            return 1u;
        --v;
        v |= v >> 1u;
        v |= v >> 2u;
        v |= v >> 4u;
        v |= v >> 8u;
        v |= v >> 16u;
        return v + 1u;
    }

    [[nodiscard]] inline uint32_t
    normalizeShadowTileResolution(uint32_t requested_resolution, uint32_t page_resolution) noexcept
    {
        const uint32_t req = std::max(requested_resolution, 1u);
        const uint32_t page = std::max(page_resolution, 1u);
        return std::min(roundUpPow2(req), page);
    }

    /// Largest power-of-two tile size at which @p required_tiles tiles still fit
    /// in a @p page_count × (@p page_resolution)² atlas.
    [[nodiscard]] inline uint32_t chooseResolutionForRequiredTiles(
        uint32_t requested_resolution,
        uint32_t page_resolution,
        uint32_t page_count,
        uint32_t required_tiles
    ) noexcept
    {
        const uint32_t page_res = std::max(page_resolution, 1u);
        const uint32_t pages = std::max(page_count, 1u);
        const uint32_t need_tiles = std::max(required_tiles, 1u);

        uint32_t resolution = normalizeShadowTileResolution(requested_resolution, page_res);
        while (true)
        {
            const uint32_t tiles_per_axis = std::max(page_res / std::max(resolution, 1u), 1u);
            const uint64_t capacity = static_cast<uint64_t>(tiles_per_axis) * static_cast<uint64_t>(tiles_per_axis) *
                                      static_cast<uint64_t>(pages);
            if (capacity >= need_tiles)
                return resolution;
            if (resolution <= 1u)
                return 1u;
            resolution >>= 1u;
        }
    }

    /// Shelf allocator over a page_count-layer atlas, rebuilt every frame.
    ///
    /// ⚠️ A tile's position is "the Nth allocation of this size" — placement is a
    /// pure function of ALLOCATION ORDER. That makes the caller responsible for
    /// feeding a stable order; see orderCastersForPlacement() and the invariant in
    /// this file's header comment.
    class ShadowAtlasPacker
    {
    public:
        ShadowAtlasPacker(uint32_t page_resolution, uint32_t page_count)
            : page_resolution_(std::max(page_resolution, 1u)), pages_(page_count)
        {
        }

        [[nodiscard]] uint32_t pageResolution() const noexcept
        {
            return page_resolution_;
        }

        [[nodiscard]] bool allocate(uint32_t requested_resolution, ShadowTileAllocation& out)
        {
            uint32_t tile_res = std::max(requested_resolution, 1u);
            tile_res = std::min(roundUpPow2(tile_res), page_resolution_);

            for (uint32_t layer = 0; layer < static_cast<uint32_t>(pages_.size()); ++layer)
            {
                auto& page = pages_[layer];
                uint32_t x = page.cursor_x, y = page.cursor_y, row_h = page.row_h;

                if (x + tile_res > page_resolution_)
                {
                    x = 0;
                    y += row_h;
                    row_h = 0;
                }
                if (y + tile_res > page_resolution_)
                    continue;

                page.cursor_x = x + tile_res;
                page.cursor_y = y;
                page.row_h = std::max(row_h, tile_res);

                out.layer = layer;
                out.x = x;
                out.y = y;
                out.resolution = tile_res;
                const float inv_page = 1.0f / static_cast<float>(page_resolution_);
                out.uv_scale =
                    Eigen::Vector2f(static_cast<float>(tile_res) * inv_page, static_cast<float>(tile_res) * inv_page);
                out.uv_bias = Eigen::Vector2f(static_cast<float>(x) * inv_page, static_cast<float>(y) * inv_page);
                return true;
            }
            return false;
        }

    private:
        struct PageState
        {
            uint32_t cursor_x{0}, cursor_y{0}, row_h{0};
        };
        uint32_t page_resolution_{1};
        std::vector<PageState> pages_{};
    };

    /// Allocate one tile, halving the request until it fits or hits the floor.
    [[nodiscard]] inline bool
    allocateTileWithFallback(ShadowAtlasPacker& packer, uint32_t requested_resolution, ShadowTileAllocation& out_tile)
    {
        const uint32_t page_resolution = packer.pageResolution();
        const uint32_t min_resolution = normalizeShadowTileResolution(kMinShadowTileResolution, page_resolution);

        uint32_t resolution = normalizeShadowTileResolution(requested_resolution, page_resolution);
        while (true)
        {
            if (packer.allocate(resolution, out_tile))
                return true;
            if (resolution <= min_resolution)
                break;
            resolution = std::max(min_resolution, resolution >> 1u);
        }
        return false;
    }

    /// Allocate six equal tiles (one cube map) atomically: a partial cube is
    /// useless, so each attempt runs against a copy and is only committed whole.
    [[nodiscard]] inline bool allocateCubeWithFallback(
        ShadowAtlasPacker& packer,
        uint32_t requested_resolution,
        std::array<ShadowTileAllocation, 6>& out_tiles
    )
    {
        const uint32_t page_resolution = packer.pageResolution();
        const uint32_t min_resolution = normalizeShadowTileResolution(kMinShadowTileResolution, page_resolution);

        uint32_t resolution = normalizeShadowTileResolution(requested_resolution, page_resolution);
        while (true)
        {
            ShadowAtlasPacker attempt = packer;
            bool allocated = true;
            for (uint32_t face = 0; face < 6; ++face)
            {
                if (!attempt.allocate(resolution, out_tiles[face]))
                {
                    allocated = false;
                    break;
                }
            }
            if (allocated)
            {
                packer = std::move(attempt);
                return true;
            }
            if (resolution <= min_resolution)
                break;
            resolution = std::max(min_resolution, resolution >> 1u);
        }
        return false;
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Placement order — the invariant this file's header states
    // ════════════════════════════════════════════════════════════════════════

    /// One caster competing for atlas space, as seen by the placement rule.
    /// `identity` is the light's stable identity (its slot, tagged by kind);
    /// `score` is the selection priority; `tile_count` is 1 for a spot, 6 for a
    /// point light; `resolution` is the tile size it asks for.
    struct ShadowCasterRequest
    {
        uint64_t identity{0};
        float score{0.0f};
        uint32_t tile_count{1};
        uint32_t resolution{1};
    };

    /// 粘滞滞回:上帧已持槽的 caster 在成员筛选里分数 ×1.5 —— 新灯要把它挤掉,
    /// 得分要高出 50%。没有这条带,图集接近饱和时边缘那一两盏灯的分数(相机
    /// 距离的连续函数)会来回换名次,而一次换人 = 幸存者集合变 = identity 序下
    /// **整张表重新编号**(slice 索引是集合内的累计位置)—— 正是「总槽位不变、
    /// 内容全洗」的 churn 主因。只影响**谁进来**,不影响**放在哪**(placement
    /// 仍按 identity),所以不会把分数序放置的旧病带回来。
    inline constexpr float kStickyScoreBoost = 1.5f;

    /// Decide WHICH casters get atlas space and in WHAT ORDER they are placed.
    ///
    /// Three steps, and the order is the whole point:
    ///
    ///   1. MEMBERSHIP — walk in descending EFFECTIVE score (score × sticky
    ///      boost for last build's holders, see kStickyScoreBoost) against a
    ///      throwaway copy of @p packer and keep the ones that fit. This is
    ///      where a near-light preference is honoured: on a full atlas the low
    ///      scorers fall off.
    ///   2. PLACEMENT ORDER — sort the survivors by identity.
    ///   3. VERIFY — re-run the allocation against a fresh copy of @p packer in
    ///      THAT order; if anyone fails, drop the lowest-scored survivor and
    ///      retry. Shelf packing is order-dependent: without this step a set
    ///      the score-order probe accepted can fail in the caller's identity-
    ///      order real allocation — a silently dropped/downgraded tile.
    ///
    /// Result: **a caster's tile depends on its identity and on which casters are
    /// present, never on their score order** — and the caller's real allocation
    /// in the returned order is guaranteed to succeed (same order, same start
    /// state as step 3).
    ///
    /// @param slice_budget  total slices available; a request needs tile_count.
    /// @param sticky_identities  SORTED identities holding a tile in the last
    ///        build (empty = no hysteresis, e.g. first build).
    /// @returns the surviving requests, in placement order.
    [[nodiscard]] inline std::vector<ShadowCasterRequest> orderCastersForPlacement(
        std::vector<ShadowCasterRequest> requests,
        const ShadowAtlasPacker& packer,
        uint32_t slice_budget,
        uint32_t slices_already_used,
        const std::vector<uint64_t>& sticky_identities = {}
    )
    {
        const auto effectiveScore = [&](const ShadowCasterRequest& r) {
            return std::binary_search(sticky_identities.begin(), sticky_identities.end(), r.identity)
                       ? r.score * kStickyScoreBoost
                       : r.score;
        };
        std::sort(requests.begin(), requests.end(), [&](const ShadowCasterRequest& a, const ShadowCasterRequest& b) {
            const float ea = effectiveScore(a), eb = effectiveScore(b);
            if (ea != eb)
                return ea > eb;
            return a.identity < b.identity; // total order — no ties left to chance
        }
        );

        const auto tryAllocate = [](ShadowAtlasPacker& p, const ShadowCasterRequest& r) {
            if (std::max(r.tile_count, 1u) == 6u)
            {
                std::array<ShadowTileAllocation, 6> t{};
                return allocateCubeWithFallback(p, r.resolution, t);
            }
            ShadowTileAllocation t{};
            return allocateTileWithFallback(p, r.resolution, t);
        };

        // ① MEMBERSHIP(有效分数降序;survivors 因此天然按分数降序存放)。
        ShadowAtlasPacker probe = packer;
        uint32_t used = slices_already_used;
        std::vector<ShadowCasterRequest> survivors;
        survivors.reserve(requests.size());
        for (const ShadowCasterRequest& r : requests)
        {
            const uint32_t need = std::max(r.tile_count, 1u);
            if (used + need > slice_budget)
                continue;
            if (!tryAllocate(probe, r))
                continue;
            used += need;
            survivors.push_back(r);
        }

        // ②+③ PLACEMENT + VERIFY:按 identity 序对着新 packer 复验;装不下就
        // 剔掉分数最低的幸存者(分数序尾)重来。集合单调缩小,必然终止;n 为
        // 几十、shelf 分配是游标推进,最坏 O(n²) 无关紧要。
        std::vector<ShadowCasterRequest> ordered;
        while (!survivors.empty())
        {
            ordered = survivors;
            std::sort(ordered.begin(), ordered.end(), [](const ShadowCasterRequest& a, const ShadowCasterRequest& b) {
                return a.identity < b.identity;
            }
            );
            ShadowAtlasPacker verify = packer;
            uint32_t verify_used = slices_already_used;
            bool all_fit = true;
            for (const ShadowCasterRequest& r : ordered)
            {
                const uint32_t need = std::max(r.tile_count, 1u);
                if (verify_used + need > slice_budget || !tryAllocate(verify, r))
                {
                    all_fit = false;
                    break;
                }
                verify_used += need;
            }
            if (all_fit)
                return ordered;
            survivors.pop_back();
        }
        return {};
    }

} // namespace lux::render
