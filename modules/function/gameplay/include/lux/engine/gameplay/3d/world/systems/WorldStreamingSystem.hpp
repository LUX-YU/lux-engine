#pragma once
/**
 * @file WorldStreamingSystem.hpp
 * @brief Cell 级世界流送(UE5 World Partition 式)—— 大世界① 增量2 W2a.
 *
 * 把世界切成均匀的 2D XY cell(spatial-hash:worldPos→cellId,**不是树**;
 * Z 跨满高),按 cell 到流送源(主相机)的距离决定 cell **active / dormant**。
 * dormant cell 里的可渲染实体被打上 [[RenderDormantComponent]] —— entt→GPU
 * 桥的 mesh 适配器据此把它们挡在 GPU 之外并回收其实例,于是 **GPU 内存随活跃
 * cell 数缩放**(= 大世界真正的可扩展性杠杆,见设计文档 §1.5)。
 *
 * 这是 client 侧(gameplay)的实体生命周期门控,**不是** render 模块那条每帧
 * GPU 粗剔(后者是 lux::render::SpatialCullGrid,按 slot mask 工作,保留)。两层
 * 各用各的距离、职责分明:本系统决定"几何进不进 GPU 场景"(载/卸,粗、滞回);
 * 逐实例 frustum+HZB cull + W1 mask 决定"已载集的每帧可见性"(细、即时)。
 *
 * 用法(host 显式驱动,像 RenderableSystem):在 World::tick(填好 WorldTransform)
 * 之后、RenderableSystem::update(entt→GPU 桥)之前调用 update(),传入流送源
 * (相机世界位)。bridge 的 reap 紧跟其后把新休眠实体的实例拆掉。
 *
 * architecture-fit(见设计文档 §7,明确不做):不建 per-instance 八叉树 /
 * two-level GPU cull / 视锥级 cell 剔除 —— cell 用**距离**激活(为流送),
 * 视锥 / 遮挡交给每帧逐实例 cull + 双缓冲 HZB。
 */

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/gameplay/world/systems/AssetLoadFn.hpp>   // AssetLoadFn / asset_id_t

namespace lux::meta { class EntityRegistry; }

namespace lux::gameplay::d3
{
    using lux::gameplay::AssetLoadFn;   // async-load hook — stays in the gameplay core

    class LUX_FUNCTION_PUBLIC WorldStreamingSystem
    {
    public:
        struct Params
        {
            float cell_size{128.0f};     ///< cell 边长(世界单位;引擎默认~128m)
            float load_range{512.0f};    ///< cell 中心进入此半径 → 唤醒(active)
            float unload_range{768.0f};  ///< cell 中心超出此半径 → 休眠(dormant)
            /// 距离前瞻预取半径(INC-2):落在 [unload, prefetch_range) 的 **dormant**
            /// cell —— 不渲染,但提前后台加载其资产数据,使 cell 真正激活时数据已就位
            /// → 消除 pop-in。须 > unload_range 才形成非空预取带;<= unload_range = 关预取。
            float prefetch_range{1024.0f};
            /// CPU 数据驱逐(W2c):cell 连续处于驱逐带(dormant 且 d2 >= max(unload,
            /// prefetch_range))超过此帧数 → 其实体资产成为驱逐候选(EditorScene 据
            /// evictableAssets() 在确认全局无引用后 unloadData,释放 CPU 内存)。0 = 关驱逐。
            uint32_t evict_age_frames{120};
            bool  enabled{true};         ///< 关 → 全 active(清掉所有 dormant 标签)
        };
        /// load_range <= unload_range 形成滞回带:带内保持当前状态,防止相机在
        /// cell 边界抖动时反复载/卸(标签的"在不在"本身就是滞回状态,无需额外存储)。

        WorldStreamingSystem() = default;

        void          setParams(const Params& p) noexcept { params_ = p; }
        const Params& params() const noexcept             { return params_; }
        void          setEnabled(bool e) noexcept         { params_.enabled = e; }
        [[nodiscard]] bool enabled() const noexcept        { return params_.enabled; }

        /// 注入"请异步加载资产数据"回调(= 渲染桥用的同一 [[AssetLoadFn]] 函数注入
        /// seam,gameplay 不碰 stdexec)。设了它,update() 会对预取带的 dormant cell
        /// 实体主动 requestLoad(幂等);不设 = 不预取(纯响应式,cell 激活当帧才载)。
        void setLoadSink(AssetLoadFn fn) noexcept { load_sink_ = std::move(fn); }

        /// W2c:本帧的 CPU 数据驱逐候选资产 id(落在驱逐带且超龄的 cell 的实体资产)。
        /// EditorScene 遍历它,对每个**全局无引用**(RenderableSystem::isAssetReferenced
        /// == false)的 id 调 AssetManager::unloadData 释放 CPU 内存。每次 update 重建;
        /// 可能含重复 id(unloadData/无引用判定均幂等)。evict_age_frames==0 → 恒为空。
        [[nodiscard]] std::span<const lux::asset::asset_id_t> evictableAssets() const noexcept
        {
            return evictable_assets_;
        }

        /// 每帧:对带 WorldTransform + (MeshComponent | SkeletalMeshComponent) 的
        /// 实体按其 cell 到任一 @p sources 的距离(滞回)toggle RenderDormantComponent。
        /// @p sources = 流送源世界位 (XYZ);空 或 disabled → 全 active(清标签)。
        /// 静态几何的 cell 归属不变,只有相机移动改变 active 集 —— 但距离阈值是连续的,
        /// 故每帧全量评估(O(实体数));大场景再上 cell→实体分桶优化(W2 后续)。
        void update(lux::meta::EntityRegistry&            registry,
                    std::span<const std::array<float, 3>> sources);

        // ── 纯函数核心(无 entt / 无状态,CPU 可单测) ──
        /// 地面两轴 → cell 整数格坐标(spatial-hash,非树;floor 含负数)。两个入参是
        /// 水平面的两个轴 —— 引擎 Y-up,故调用方传 (world.x, world.z),Y/高度被跨越。
        static void cellCoord(float world_x, float world_z, float cell_size,
                              int32_t& out_cx, int32_t& out_cy) noexcept;
        /// cell (cx,cy) 中心到任一 source 的最小**地面 X-Z** 平方距离(Y-up:用 source
        /// 的 x 与 z,跨越 Y/高度);sources 空 → 0(视作贴在源上 = active)。平方距离避
        /// 免开方,调用方与 range*range 比较。
        [[nodiscard]] static float cellMinDist2(
            int32_t cx, int32_t cy, float cell_size,
            std::span<const std::array<float, 3>> sources) noexcept;

        // ── 调试统计(上次 update 的结果;验证流送:活跃实体数随相机远离下降) ──
        [[nodiscard]] uint32_t activeCount()  const noexcept { return stat_active_; }
        [[nodiscard]] uint32_t dormantCount() const noexcept { return stat_dormant_; }
        [[nodiscard]] uint32_t totalCount()   const noexcept { return stat_total_; }

    private:
        struct CellKey
        {
            int32_t x{0};
            int32_t y{0};
            bool operator==(const CellKey&) const noexcept = default;
        };
        struct CellKeyHash
        {
            std::size_t operator()(const CellKey& k) const noexcept
            {
                return std::hash<std::uint64_t>{}(
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) << 32)
                    | static_cast<std::uint32_t>(k.y));
            }
        };

        Params params_{};

        // INC-2:预取回调(函数注入 seam)。设了才预取。
        AssetLoadFn load_sink_{};

        // 持久 cell→dormant 状态:滞回的记忆(跨帧)。每帧 update 按它 + 距离推进。
        std::unordered_map<CellKey, bool, CellKeyHash> cell_dormant_;

        // W2c:cell 连续处于驱逐带的帧龄(防抖,跨帧)+ 本帧驱逐候选(重建)。
        std::unordered_map<CellKey, uint32_t, CellKeyHash> cell_evict_age_;
        std::vector<lux::asset::asset_id_t>                evictable_assets_;

        uint32_t stat_active_{0};
        uint32_t stat_dormant_{0};
        uint32_t stat_total_{0};
    };

} // namespace lux::gameplay::d3
