#pragma once
/**
 * @file SpatialCullGrid.hpp
 * @brief Cell 级空间剔除网格(spatial-cell-grid coarse cull)—— 工作流① 增量2 W1.
 *
 * 把空间切成均匀的 2D XY cell(spatial-hash:worldPos→cellId,**不是树**;Z 跨满高),
 * 按 cell 到剔除源(主相机)的距离激活 / 休眠。dormant cell 的实例通过 per-slot active
 * mask 被挡在每帧 GPU cull dispatch 之外 —— 压低"任一时刻活跃实例数"(= 大世界真正的
 * 可扩展性杠杆,见设计文档 §1.5)。大世界流送是它的典型客户,但任何想按距离/cell 粗剔
 * 的场景都能用它(机制,非业务)。
 *
 * 这是一个 SceneResourceRegistry 组件(像 InstanceResources),由 SpatialCullFeature 在
 * onFrameBegin 驱动 update(主相机就绪后、feature cull 之前)。它**不取代**现有的逐实例
 * frustum+HZB cull —— 那是每帧可见性(UE5 传统网格的做法,保留);空间网格只负责按距离
 * 把休眠 cell 的实例从候选集里剔掉。视锥 / 遮挡仍由逐实例 cull + 双缓冲 HZB 处理。
 *
 * 见 .internal/world-partition-design-2026-06-17.md(§3 设计 / §5 分阶段 / §6 接入点 /
 * §7 architecture-fit:明确不做 per-instance 八叉树 / two-level GPU cull / 视锥级 cell 剔除)。
 */

#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    class DeviceContext;
    class DeferredDestroyQueue;
    class InstanceResources;

    // =========================================================================
    //  SpatialCullGrid
    // =========================================================================
    class LUX_FUNCTION_PUBLIC SpatialCullGrid
    {
    public:
        struct InitInfo
        {
            DeviceContext*        device_context{nullptr};
            DeferredDestroyQueue* deferred_queue{nullptr};
            uint32_t              frames_in_flight{2};      ///< 决定 mask 环 slot 数(FIF+1)
            uint32_t              initial_capacity{4096};   ///< 预分配的 per-slot mask 元素数
            float                 cell_size{128.0f};        ///< cell 边长(世界单位;引擎默认~128m)
            float                 cull_distance{512.0f};    ///< 剔除距离(cell 距相机超此 → 休眠)
        };

        SpatialCullGrid() = default;
        ~SpatialCullGrid();

        SpatialCullGrid(const SpatialCullGrid&)            = delete;
        SpatialCullGrid& operator=(const SpatialCullGrid&) = delete;

        void init(const InitInfo& info);
        void shutdown();

        /// 一个 alive 实例的世界 XY(归 cell 只用 bsphere 中心的 XY;cell 跨满 Z)。底层
        /// update 直接吃它,使 SpatialCullGrid 不强耦合 InstanceResources 具体类型(也让
        /// headless GPU 测试免去构造完整 InstanceResources)。
        struct InstanceXY
        {
            uint32_t slot;
            float    x;
            float    y;
        };

        /// 每帧:实例按世界 bsphere 中心归 cell + 距离激活 cell + 填 per-slot active mask +
        /// 上传到当前环 slot。@p serial 去重:同一帧(同 serial)多次调用只计算一次、共享
        /// 一个 buffer(Forward / Deferred / Shadow 三个 cull 都 import 同一个 mask)。
        /// @p cameras = active view 的世界相机位(XYZ);空 → 全 active(不剔,等价现状)。
        /// 动态实例天然友好:cell 归属用 hash,实例移动 = O(1) 重哈希(每帧重建)。
        void update(uint64_t                                  serial,
                    std::span<const std::array<float, 3>>     cameras,
                    const InstanceResources&                  instances);

        /// 底层 update:直接喂 alive 实例的 (slot, world xy)。便利重载从 InstanceResources
        /// 提取 bsphere 后委托此函数;serial 去重 / 环推进 / mask 上传都在这里。
        void update(uint64_t                                  serial,
                    std::span<const std::array<float, 3>>     cameras,
                    std::span<const InstanceXY>               alive,
                    uint32_t                                  slot_count);

        /// 当前帧的 per-slot active mask buffer(uint per slot;active=1 / dormant=0)。
        /// 保留供测试 / 兼容。init 后总返回有效 buffer(预填全 active)。
        [[nodiscard]] VkBuffer activeMaskBuffer() const noexcept;

        /// 当前帧 active mask buffer 的 GPU device address(buffer-device-address)。
        /// cull dispatch 把它经 push-constant 传给 mesh_cull_unified.comp,shader 用
        /// buffer_reference 读 —— 取代了固定的 descriptor binding 8,使粗剔可 opt-in:
        /// SpatialCullGrid 不存在时调用方传 0,shader 跳过 mask 读(= 全 active)。
        /// 未 init / 当前 slot 为空 → 返回 0(哨兵)。
        [[nodiscard]] uint64_t activeMaskAddress() const noexcept;

        /// mask 覆盖的 slot 数(= 上次 update 的 instances.slotCount())。
        [[nodiscard]] uint32_t maskSlotCount() const noexcept { return mask_slot_count_; }

        /// 读回当前环 slot 的 host-mapped mask(active=1 / dormant=0),仅供测试核对 GPU
        /// 上传内容。host-coherent 映射,读到的就是本帧 cull dispatch 看到的。未 init → null。
        [[nodiscard]] const uint32_t* gpuMaskMappedForTest() const noexcept;

        // ── 运行期可调 ──
        void setCellSize(float s) noexcept;
        void setCullDistance(float r) noexcept;
        void setEnabled(bool e) noexcept { enabled_ = e; }   ///< 关闭 → 全 active(对照粗剔收益)

        [[nodiscard]] bool  enabled() const noexcept       { return enabled_; }
        [[nodiscard]] float cellSize() const noexcept      { return cell_size_; }
        [[nodiscard]] float cullDistance() const noexcept  { return cull_distance_; }

        // ── 纯函数核心(无 GPU / 无状态依赖,CPU 可单测) ──
        /// 世界 XY 坐标 → cell 整数格坐标(spatial-hash 的 hash 部分,非树)。
        static void cellCoord(float world_x, float world_y, float cell_size,
                              int32_t& out_cx, int32_t& out_cy) noexcept;
        /// cell (cx,cy) 是否在任一相机的剔除范围内。范围 = cull_distance + 一个 cell
        /// 余量(保守:用 cell 中心 2D 距离,宁可多激活不误剔)。cameras 空 → true(不剔)。
        static bool cellInRange(int32_t cx, int32_t cy, float cell_size, float cull_distance,
                                std::span<const std::array<float, 3>> cameras) noexcept;

        // ── 调试统计(上次 update 的结果;用于验证粗剔:活跃实例数随相机远离下降) ──
        [[nodiscard]] uint32_t activeInstanceCount() const noexcept { return stat_active_instances_; }
        [[nodiscard]] uint32_t totalInstanceCount()  const noexcept { return stat_total_instances_; }
        [[nodiscard]] uint32_t activeCellCount()     const noexcept { return stat_active_cells_; }
        [[nodiscard]] uint32_t totalCellCount()      const noexcept { return stat_total_cells_; }

    private:
        // cell 坐标键(2D XY 整数格)。
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

        void recomputeMask(std::span<const std::array<float, 3>> cameras,
                           std::span<const InstanceXY>           alive,
                           uint32_t                              slot_count);
        void uploadMask();         ///< grow 当前环 slot buffer if needed + memcpy mask_
        void destroyRing() noexcept;

        DeviceContext*        device_ctx_{nullptr};
        DeferredDestroyQueue* deferred_queue_{nullptr};

        float cell_size_{128.0f};
        float cull_distance_{512.0f};
        bool  enabled_{true};
        bool  initialized_{false};

        // ── CPU mask + cell active 缓存(每帧重建) ──
        std::vector<uint32_t>                              mask_;            ///< per-slot active flag
        std::unordered_map<CellKey, uint8_t, CellKeyHash>  cell_active_;     ///< lazy cell→active 缓存
        std::vector<InstanceXY>                            extract_scratch_; ///< 便利 update 提取 alive 的复用缓冲
        uint32_t mask_slot_count_{0};

        // ── per-FIF active mask GPU 环 ──
        // 每个 FIF 一个独立 VkBuffer(RG importBuffer 是整-buffer 模型,offset 0),
        // CPU-mapped host-coherent。复用的环 slot 至少 (FIF+1) 帧前写过 → GPU idle,
        // 写它不会撕裂在飞帧。参考 InstanceResources mdc_info 环。
        std::vector<VkBuffer>      ring_buffers_;
        std::vector<VmaAllocation> ring_allocs_;
        std::vector<void*>         ring_mapped_;
        std::vector<VkDeviceSize>  ring_sizes_;
        uint32_t ring_cursor_{0};
        uint32_t current_slot_{0};
        uint64_t last_upload_serial_{~0ull};

        // ── 调试统计 ──
        uint32_t stat_active_instances_{0};
        uint32_t stat_total_instances_{0};
        uint32_t stat_active_cells_{0};
        uint32_t stat_total_cells_{0};
    };

} // namespace lux::render
