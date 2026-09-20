# Simulation：世界演进、线程 owner 与稳定点

Simulation 决定活动世界如何演进。World 描述和 Registry 存储内容，具体 System 执行演化或派生逻辑，Scene 完成它们的装配。

本文说明模块责任与推进协议，不记录阶段验收数字或构建目录。

## 组合关系

```text
WorldDescription：恢复内容与能力
Registry：当前 Entity 与组件
Simulation：系统、任务图、时钟与执行状态
Scene：上述对象及 SceneSystem 的生命周期装配
```

Simulation 不是窗口主循环，也不是 Renderer。没有 RenderSystem、Camera 或 Vulkan，Simulation 仍然可以执行逻辑、物理和脚本。

内置系统放在 `builtin_systems/`，例如 Transform、Script、Physics2D。渲染集成是可选的 SceneSystem，位于 `engine/scene/builtin_systems/render/`。

## Editor 中的线程责任

Editor 的 Main 拥有并推进作者 Scene 和活动 Run Scene。不能把整个 Scene 的生命周期放进一个长期占用 Process worker 的任务，再让 UI 间接等待它。

TaskGraph／TaskExecutor 负责按访问约束执行系统任务。Main owner 不等于所有具体计算永远只能同步运行；可分离计算应通过已有任务设施执行，并在明确稳定点采用结果。

与此同时，不能宣称“Simulation 再慢也不会影响 Editor 帧率”。如果 Main 同步等待长计算，它就不能及时生产 UI Frame；渲染线程消费完已提交工作后也会等待。

当前 Editor Run 由 Main 发起单步，使用 caller 执行器。这不等同于已经提供任意系统负载下的异步单步或硬实时保证。

## 演化和派生

[`Simulation`](composition/include/lux/engine/simulation/Simulation.hpp) 区分 `EVOLUTION` 与 `DERIVATION`，模式在构造时确定。

- 作者内容编辑需要派生更新，例如从局部 Transform 得到 WorldTransform。
- 活动 Run 推进演化系统与模拟时钟。
- 刷新派生数据不应冒充执行了一次模拟步。
- 不能通过直接复用一个不兼容模式的实例，绕过系统启停、时钟及任务图约束。

作者 Scene 和 Run 可以是独立实例，同时在同一个编辑器视口切换显示。隔离运行内容和决定显示位置是两项独立设计。

## 时钟与现实节奏

固定模拟步长描述一次 Simulation 演化采用的时间增量。它与主循环等待时间、GPU 帧间隔、真实世界时间不是同一个量。

现有固定步长 Run 的约束是：每轮有限推进，计算和发布时间计入现实周期；落后时不积累无限追赶任务，也不重复执行同一步。

未来固定步进、真实时间、外部驱动等时基应分别表达其语义。有限的内置类型可以使用 concept 与 variant；本文不宣称这些时基已全部实现，也不借此扩展当前 Run 模式。

## 稳定点与发布背压

```text
一次演化或作者派生完成
  → 通用稳定点
  → 已安装 SceneSystem 提取变化
  → 必要更新被接纳
  → 允许继续下一次演化
```

稳定点是通用生命周期事实，不是 Scene 核心的专用渲染阶段。

必要发布遇到背压时，保留待接纳数据并返回 Main；后续正常推进继续尝试。Main 不能阻塞等待一个也需要自己消费的队列。

Pause、背压和 Stop 都应允许主循环继续处理回复、输入和资源退出。渲染器绘制最后采用的状态，不通过无限推进 Simulation 或无界堆积更新掩盖不同速问题。

## 查询与脚本

运行时查询返回 `ecs::Entity`。查询不要求 WorldObjectId、Camera 或 Renderer。

查询和写操作的关系通过系统任务依赖及数据版本明确：读哪一轮 Transform 和索引、何时允许结构变更、何时采用查询结果。不能在查询函数中偷偷执行完整 Simulation 来修补版本问题。

脚本接线复用已有 Ability、生成代码与 provider 生命周期。脚本射线查询不调用 Editor 选择 API，也不返回内部组件指针。

## 失败记录

时钟已经采用一步，不代表所有系统、稳定点和发布都成功。失败结果必须同时保留：

- 实际时钟与步号。
- 已完成的阶段。
- 原始失败及所属系统。
- 已发生的外部效果和待收尾资源。

首个原始失败不能被后续停止错误覆盖。失败和关闭进度是两个维度：资源已经退出，不意味着本次运行应改成成功。

相关说明：[ECS](ecs/README.md)、[Scene](../../scene/README.md)、[Process](../../process/README.md)、[渲染集成](../../scene/builtin_systems/render/README.md)。
