# 10 — Backend Scene/View/Target、ViewGroup 与单帧输出

## 10.1 Backend-neutral Scene/View/Target

`Scene` 的唯一业务职责：资源域关联、逻辑 Scene 值和 Scene-owned View 容器；不持 Feature、Graph、Native PSO。`View` 包含 Camera/History validity/epoch、ResourceDomainId、RenderEffectId、启用状态等，不存可自由修改的“当前 GraphId 权威”。Backend 可为 View 维护派生 graph/context handle，但在 Effect 版本变更时以统一发布事实驱动。`Target` 是独立 output 语义对象：`TargetDesc` 指定 Offscreen/Surface、颜色/深度/额外语义输出、采样数/尺寸/resize；`TargetLayer`/多 View input 描述 compositor graph 的显式层顺序。三个公共类型不能包含 Vk 头和 WSI owner。

## 10.2 `execute(graph,view,target)` 的精确定义

一次成功接纳 `execute()` 是一张完整 Graph 的执行任务；其 Pass 已含全部物理中间过程，最终写出 Target 的完整输出（GPU completion 后可显示/readback），不允许在外部偷偷执行独立的 Tonemap、Lighting 或 UI 后处理。普通 View→Target 一对一时使用三参接口；同一 Target 多个 View 组合要求 Effect Graph **显式声明多个 View 输入和 Composite Pass**，然后一个完成目标的 Graph 进行执行，内部可使用借来的 `span<View>`。若只是把已完成 View Target 最终画面复制给第二个 Target，必须通过明确的目标输出引用/导出方式或在该目标 Graph 内表达，而不是重做每个 Scene Pass。

跨帧 history 是显式 persistent import/export；别的 Offscreen Target 的**最终结果**是独立存在的实际 GPU 资产，可在别的 Graph 显式导入。在这种受控情形，Backend 需要 Target job 的 readiness/dependency 次序，但不能变成对同一 Graph 的中间 texture 进行额外 Graph 调度。

## 10.3 ViewGroup 精确语义

`RenderGraphId → 该编译产物下的准备执行 Job 集合`；Job 是值型 `(GraphId, TargetId, ViewIds/Layer inputs, ContextHandle, output operation)`。为保持必要 Target/Layers 依赖，分组是合法顺序里的**连续相容段**，相同 GraphId 可以出现在多个不相邻的段，不要求为此复制 Native Code 或增加第二套 Graph instance table。只有真实完全独立的 Job 才可安全重新排序改善 Pipeline/Descriptor locality。稳定拓扑在 Effect/View/Target 变更时冷态更新索引，不每帧重新创建 map/vector。

```cpp
// 实际 executor API 可以有 Frame-bound target acquisition；以下仅展示责任
prepareFrame(); // acquire/preflight + ready/queue capacity of ALL jobs
for (auto& group : view_groups) {
    auto& graph = graphs.get(group.graph_id);
    for (auto job : group.jobs()) {
        executor.execute(graph, job.views, targets.get(job.target), contexts.get(job.context));
    }
}
finishTargetsAndPresent(); // only WSI finalization/export; no hidden GPU rendering graph
```

## 10.4 GraphId 与 Effect 更新绑定

View 的唯一公共选择是 `effect_id`。Backend 根据 Effect active revision、Device enabled caps、Target compatibility 和 Shader/Graph identity 获取 GraphId、Context，并放进分组。Effect 修改可能一对多新 Graph 编译；候选先对每个当前使用 Effect 的 View 验证 Native Graph/资源/History/fallback/Slot，全部成功才整体切换。旧 Graph/Context 在途 GPU 工作不受影响。

## 10.5 共享与独立执行状态

Scene A/B 的 View 可以共享同一 Graph code/PSO/Shader，不共享 Scene 数据、View Camera/HZB/TAA 或 Descriptor Instance。Scene-scope Pass 仅在**相同 Scene、真实 inputs/revisions/side-effects 可证**的 View 间复用。View-scope work 如 Cull/HZB/GBuffer 各自执行；物理 multiview 必须按实际设备和 shader/array-view 支持另行选择，有 separate-pass fallback。

## 10.6 Target Size 与 History

Graph 可按 Target-independent layout 复用，但 Format/Samples/LocalRead/必要 Relative Extent 变化时须产生兼容 Native variant。View 历史关联的 Native resource 必须在 resize、camera cut、dynamic resolution、Effect topology/History ABI change 时明确更新 epoch/fallback，不能用 Scene revision 假装 History 有效。一个 View 面对两个不同尺寸 Target 需要 Context 区分，不要求给 View 复制两个 Camera/Scene 业务权威。

## 10.7 Runtime 独立帧节奏与故障

Backend 即便 PROGRAM 空也按合法 Target deadline 渲染，CPU Sim 10Hz + Render 144Hz 是明确 Golden。所有 Target inactive/minimized 时应无无意义空转。Open/Close/Resize/Stop/Device Lost 的 GPU/WSI completion 真实受证，Failed resize/Graph update last-good 不被破坏。Capacities 明确 bounded，实际 FIFO/Views/Native slots 不等于 `capacity.views * FIF` 的无条件物理预分配。
