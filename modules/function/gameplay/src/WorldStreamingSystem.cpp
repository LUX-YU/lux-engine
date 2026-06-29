/**
 * @file WorldStreamingSystem.cpp
 * @brief 见 WorldStreamingSystem.hpp —— cell 级世界流送(GPU 桥门控,W2a)。
 */

#include "lux/engine/gameplay/3d/world/systems/WorldStreamingSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entt

#include "lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/MeshComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/RenderDormantComponent.hpp"

namespace lux::gameplay::d3
{
    // =========================================================================
    //  Pure cell math (no entt / no state — CPU-unit-testable)
    // =========================================================================
    void WorldStreamingSystem::cellCoord(float world_x, float world_z, float cell_size,
                                         int32_t& out_cx, int32_t& out_cy) noexcept
    {
        const float inv = 1.0f / cell_size;
        out_cx = static_cast<int32_t>(std::floor(world_x * inv));
        out_cy = static_cast<int32_t>(std::floor(world_z * inv));
    }

    float WorldStreamingSystem::cellMinDist2(
        int32_t cx, int32_t cy, float cell_size,
        std::span<const std::array<float, 3>> sources) noexcept
    {
        if (sources.empty())
            return 0.0f;   // no streaming source → treat as on-source = active
        const float ccx = (static_cast<float>(cx) + 0.5f) * cell_size;
        const float ccz = (static_cast<float>(cy) + 0.5f) * cell_size;
        float best = std::numeric_limits<float>::max();
        for (const auto& s : sources)
        {
            // Ground-plane X-Z distance (engine is Y-UP → the Y/height axis is
            // spanned, NOT used for cell distance). Source is {x,y,z}; use x + z.
            const float dx = ccx - s[0];
            const float dz = ccz - s[2];
            const float d2 = dx * dx + dz * dz;
            best = std::min(best, d2);
        }
        return best;
    }

    // =========================================================================
    //  Per-frame update
    // =========================================================================
    void WorldStreamingSystem::update(lux::meta::EntityRegistry&            registry,
                                      std::span<const std::array<float, 3>> sources)
    {
        stat_active_ = stat_dormant_ = stat_total_ = 0;
        evictable_assets_.clear();   // W2c:本帧驱逐候选每帧重建

        // Disabled or no streaming source → everything active. Wipe every
        // dormant tag (so toggling streaming off restores the full render set)
        // and forget the hysteresis memory, then just tally the renderables.
        if (!params_.enabled || sources.empty())
        {
            registry.clear<RenderDormantComponent>();
            cell_dormant_.clear();
            cell_evict_age_.clear();   // W2c:关流送 → 帧龄清零
            uint32_t n = 0;
            registry.view<MeshComponent, WorldTransformComponent>().each(
                [&](entt::entity, MeshComponent&, WorldTransformComponent&) { ++n; });
            registry.view<SkeletalMeshComponent, WorldTransformComponent>().each(
                [&](entt::entity, SkeletalMeshComponent&, WorldTransformComponent&) { ++n; });
            stat_active_ = stat_total_ = n;
            return;
        }

        const float cs      = (params_.cell_size > 0.0f) ? params_.cell_size : 128.0f;
        const float load2   = params_.load_range * params_.load_range;
        // Clamp unload >= load so the hysteresis band is never inverted.
        const float unload  = std::max(params_.unload_range, params_.load_range);
        const float unload2 = unload * unload;
        // INC-2 预取带:prefetch_range > unload 才非空;否则 -1 = 关预取。
        const float prefetch2 = (params_.prefetch_range > unload)
            ? params_.prefetch_range * params_.prefetch_range
            : -1.0f;
        // W2c 驱逐带:超出"保持驻留"半径 = max(unload, prefetch_range);更远 + 超龄 → 驱逐。
        const float evict_radius = std::max(unload, params_.prefetch_range);
        const float evict2       = evict_radius * evict_radius;
        const bool  evict_on     = params_.evict_age_frames > 0;

        // Decide each cell at most once per update. The persistent cell_dormant_
        // holds the PREVIOUS frame's state (the hysteresis memory); `decided`
        // is this frame's freshly computed verdict so every entity sharing a
        // cell reads the same answer even as we write the new state back.
        struct CellState { bool dormant; bool prefetch; bool evictable; };
        std::unordered_map<CellKey, CellState, CellKeyHash> decided;
        decided.reserve(cell_dormant_.size());

        auto cellState = [&](int32_t cx, int32_t cy) -> CellState
        {
            const CellKey key{cx, cy};
            if (const auto it = decided.find(key); it != decided.end())
                return it->second;

            const float d2 = cellMinDist2(cx, cy, cs, sources);
            const auto  pit = cell_dormant_.find(key);
            const bool  prev_dormant = (pit != cell_dormant_.end()) ? pit->second : false;

            // Hysteresis: an active cell sleeps only once beyond unload_range;
            // a dormant cell wakes only once back inside load_range. Inside the
            // band [load,unload] the cell keeps its previous state — no thrash.
            const bool dormant = prev_dormant ? (d2 >= load2) : (d2 > unload2);
            // 预取带:dormant 且仍在 prefetch_range 内 → 想要其数据提前就位(不渲染)。
            // active cell 无需预取(响应式桥当帧就载)。
            const bool prefetch = dormant && (prefetch2 >= 0.0f) && (d2 < prefetch2);

            // W2c:驱逐带 = dormant 且超出 evict_radius。帧龄防抖(连续在带内才计数),
            // 一次/cell/帧(本分支因 decided 记忆每 cell 每帧只走一次)。超龄 → 驱逐候选。
            const bool in_evict = dormant && (d2 >= evict2);
            bool evictable = false;
            if (evict_on)
            {
                uint32_t& age = cell_evict_age_[key];
                age = in_evict ? (age < 0xffffffffu ? age + 1 : age) : 0;
                evictable = in_evict && (age >= params_.evict_age_frames);
            }

            const CellState st{dormant, prefetch, evictable};
            decided.emplace(key, st);
            cell_dormant_[key] = dormant;
            return st;
        };

        auto applyDormant = [&](entt::entity e, bool dormant)
        {
            const bool tagged = registry.all_of<RenderDormantComponent>(e);
            if (dormant && !tagged)
                registry.emplace<RenderDormantComponent>(e);
            else if (!dormant && tagged)
                registry.remove<RenderDormantComponent>(e);

            if (dormant) ++stat_dormant_; else ++stat_active_;
            ++stat_total_;
        };

        // INC-2:预取带 dormant cell 的实体资产 → 经函数注入 seam 主动 requestLoad
        // (幂等去重)。沿用渲染桥同款 AssetLoadFn;不设 sink 则为空操作。
        // 预取本帧请求过的 id —— 驱逐对其让步(见下方过滤):同一资产被某 cell 预取
        // 又被另一 cell 列入驱逐时,若不让步会"载了又卸"每帧 churn。预取优先 = 想要它
        // 驻留的 cell 存在,就别驱逐。
        std::unordered_set<lux::asset::asset_id_t> prefetched_this_frame;
        auto prefetchId = [&](const lux::asset::asset_id_t& id)
        {
            if (id.is_nil()) return;
            if (load_sink_) load_sink_(id);
            prefetched_this_frame.insert(id);
        };
        // W2c:收集驱逐候选 id(EditorScene 后续确认无引用后 unloadData)。
        auto evictId = [&](const lux::asset::asset_id_t& id)
        {
            if (!id.is_nil()) evictable_assets_.push_back(id);
        };

        // The streaming system sees ALL renderables (tagged or not) so it can
        // wake dormant ones — only the GPU bridges exclude the tag.
        registry.view<MeshComponent, WorldTransformComponent>().each(
            [&](entt::entity e, MeshComponent& mc, WorldTransformComponent& wt)
            {
                int32_t cx = 0, cy = 0;
                cellCoord(wt.world(0, 3), wt.world(2, 3), cs, cx, cy);  // X-Z ground plane (Y-up)
                const CellState st = cellState(cx, cy);
                applyDormant(e, st.dormant);
                if (st.prefetch)
                {
                    prefetchId(mc.mesh_asset_id);
                    prefetchId(mc.material_asset_id);
                }
                else if (st.evictable)
                {
                    evictId(mc.mesh_asset_id);
                    evictId(mc.material_asset_id);
                }
            });
        registry.view<SkeletalMeshComponent, WorldTransformComponent>().each(
            [&](entt::entity e, SkeletalMeshComponent& sc, WorldTransformComponent& wt)
            {
                int32_t cx = 0, cy = 0;
                cellCoord(wt.world(0, 3), wt.world(2, 3), cs, cx, cy);  // X-Z ground plane (Y-up)
                const CellState st = cellState(cx, cy);
                applyDormant(e, st.dormant);
                if (st.prefetch)
                {
                    prefetchId(sc.mesh_asset_id);
                    prefetchId(sc.material_asset_id);
                    prefetchId(sc.skeleton_asset_id);
                }
                else if (st.evictable)
                {
                    // 只驱逐桥追踪的资产(mesh/material)—— isAssetReferenced 仅识别这些,
                    // 故能作驱逐守卫。skeleton 不进桥 + SkeletalAnimationResolver 对 dormant
                    // 实体仍逐帧重查(会立刻重载),故不驱逐(且骨架本身小)。
                    evictId(sc.mesh_asset_id);
                    evictId(sc.material_asset_id);
                }
            });

        // 预取优先于驱逐:同一资产被某 prefetch cell 要、又被某 evict cell 列入时,
        // 不驱逐(否则每帧载了又卸 churn)。EditorScene 再叠一道 isAssetReferenced
        // (活跃 cell)守卫 → 仅"既非活跃、又非预取"的资产才真正释放。
        if (!evictable_assets_.empty() && !prefetched_this_frame.empty())
            std::erase_if(evictable_assets_,
                [&](const lux::asset::asset_id_t& id)
                { return prefetched_this_frame.contains(id); });
    }

} // namespace lux::gameplay::d3
