#pragma once
// ============================================================================
//  SystemPhase.hpp — 帧内相位。`Schedule` 的系统表按它排序。
//
//  ── 为什么是「具名相位」而不是裸数字 ─────────────────────────────────────
//
//  数字本身不携带语义：「10 比 20 早」没说清**为什么**早，两个独立作者的包各挑
//  一个数字就会撞。相位名字是一句人能读懂的话——「在变换之前」——所以它既是排序
//  键，也是那条约束的文档。数字只是实现细节，调用方写名字。
//
//  Unreal 的 `TG_PrePhysics` / `TG_PostPhysics`、Bevy 的 `PreUpdate` / `Update` /
//  `PostUpdate`、Flecs 的 `EcsPreUpdate` / `EcsOnUpdate`、Unity DOTS 的
//  `SimulationSystemGroup` / `PresentationSystemGroup` 都是这一类。
//
//  相位现在只是拓扑排序就绪集的粗粒度优先级；精确约束由系统的 before/after
//  edge 表达并压过相位。没有显式边时，同相位仍按注册序稳定决胜。数字之间保留
//  间隔只为新增一个有清楚语义的粗阶段，不应用 `+5/+10` 编码二元依赖。
// ============================================================================

namespace lux::ecs
{
    /// 帧内相位。数值本身没有意义，只有相对大小有。
    enum : int
    {
        /// 场景尚未 READY 时唯一开放的阶段：Section 读取/生成、
        /// 分步 staging 和发布命令。宿主以 dt=0 执行到此上界，
        /// startup tickets 全部 ACTIVE 后才开放 gameplay 相位。
        kPhaseSceneLoading   = 50,

        /// 读输入、改「意图」——编辑器相机导航、玩家控制器。必须先于一切派生量。
        kPhaseInput          = 100,

        /// 派生量的**上游**：要在变换/相机把矩阵算出来之前跑完的东西。
        /// 骨骼动画的资产解析器在这里（`AnimationSystem` 要读它写的指针），
        /// 相机域的 `CameraViewSubsystem` 也在这里（绑定要先于派生量刷新落位）。
        kPhasePreTransform   = 200,

        /// 主模拟：变换、相机、动画、物理、脚本。域包的系统默认落在这里。
        kPhaseSimulation     = 300,

        /// 渲染**之前**要写完的 registry 状态，例如选中高亮标签。
        /// 渲染节点这一帧读到的就是这里写下的。
        kPhasePreRender      = 400,

        /// 扁平渲染节点的粗排序相位。每个节点都是 Schedule 的普通条目；
        /// 节点之间的细排序由 `runsBefore` / `runsAfter` 的强类型边表达。
        kPhaseRender         = 500,

        /// 渲染**之后**：世界流送的 CPU 数据驱逐（要等实例已经被摘掉）。
        kPhasePostRender     = 600,

        /// `Schedule::tick` 的默认上界 —— 跑完整帧。
        kPhaseLast           = 0x7fffffff,
    };

} // namespace lux::ecs
