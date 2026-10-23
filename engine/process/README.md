# Process：有限异步工作与结果采用

Process 提供异步执行、资产读取和内容加载设施。它不成为所有业务 owner 的容器，也不替代 Scene、Simulation 或 Renderer 的生命周期。

## 现有结构

| 子模块 | 接点 |
| --- | --- |
| Core `execution` | 复用 ExecutionRuntime、Task、TaskScope、CompletionWork、计时设施 |
| `object_execution` | ObjectScheduler：对象线程采用及目标代际保护 |
| `asset_loading` | 资产读取端点与 AssetLoadSender |
| `world_loading` | World 存储读取与 WorldPartitionLoadSender |

采用既有 sender／receiver 和 operation state，不另外建设轮询线程、调度器或事件总线。

## 适合交给 Process 的工作

资产读取／写入、模型解码、正式编译流程以及可以独立执行的有限计算。业务 owner 捕获明确输入，任务执行后将拥有型结果交回 owner 采用。

```text
调用方验证并捕获请求
  → sender 执行有限工作
  → 运输结果直接交给下一步读取、CPU 解码或上传
  → 需要业务采用时，显式切换到 Main/Object Scheduler
  → 对应线程派发 continuation，在安全点采用
  → Runtime 记录任务终态；dispatchTaskEvents 只通知监控
```

“任务完成”不等于“业务已经采用”，更不等于 GPU 完成。停止结果不能当作成功结果，失败之前已经发生的文件效果也不能因错误返回而被抹掉。

## 执行错误的跨模块表示

`execution/ExecutionError.hpp` 提供 `registerExecutionErrors()` 和 `toError(EExecutionError)`。
宿主装配时显式登记 Process 目录并处理登记冲突；转换只复制稳定错误 ID 和数值参数，
不登记描述、不查询全局目录、不分配。原 `lux.process.execution.*` 身份及恢复分类保持不变。
Editor 等消费者不再维护一份执行错误映射；Process 只依赖底层 `core::error`，不依赖 Editor。

## 与 Scene 的边界

Editor 中 Main 拥有活动 Scene。不得将 Scene 建立、无限运行循环、暂停等待和销毁全部塞进一个长期 Process worker，使该 worker 变成 Scene owner。

Simulation 的系统并行使用现有 TaskGraph 和明确访问契约。资产加载可以同时进行，但不能把可失效的 Registry 引用、Pane 指针或组件地址交给脱离 owner 生命周期的任务。

## 与空间查询的边界

分区加载 sender 处理内容读取。运行时射线检测处理当前活动实体，不隐式调用分区加载 sender。

需要读取模型几何或建立可复用局部 BVH 时，可以使用有限任务准备资源；资源未就绪的状态由实际 owner 表达。不能让一次同步查询悄悄阻塞 Main 等待磁盘和解码。

## 模型拖放

拖放是 Editor 操作。Editor 固定资产引用、放置目标、坐标及内容版本后提交读取请求；Process 只负责相应读取／解码。

任务完成后由 SceneEditor 验证原目标仍有效，再创建 Entity 和历史。Pane 隐藏或销毁不应使一个已经接纳的业务请求失去 owner。进入 Run 后也不能把作者请求的结果写入运行 World。

## 接纳与关闭

接纳拒绝不会调用 sender 工厂。需要保留可重试输入的调用方应在工厂内部移动，不能在构造工厂捕获时提前移动唯一副本。
接纳后 operation state、输入资源和代码有效期由明确 owner 保持。取消意图不是任务已停止的事实。

关闭期间继续推进必要完成，直到已接纳工作获得终态并解除借用。不能用强制结束进程或空日志代替正常收尾。

本文不新增 OOM 恢复工程；正常 IO、解码、编译和业务错误仍须准确保留。

相关说明：[World](../domain/world/README.md)、[Simulation](../domain/simulation/README.md)、[Scene Editor](../../editor/tools/scene/README.md)。

## 统一任务接口

`submit(TaskOptions, factory(TaskReporter))` 接纳成功即启动有限 Sender。
流水线自己表达读取、CPU 计算、运输完成和 Main/Object 采用边界；Runtime 不保存通用结果回调。
Task 只是非阻塞取消句柄，析构请求停止，不等待、不收取完成，也不撤销已经发生的副作用。
TaskScope 是准入与分组取消域；析构后已接纳工作仍由原 Runtime 的 stdexec Scope 清偿。
TaskRecord 保留任务身份、进度、取消和历史，不再拥有 connected OperationState。

`dispatchMainCompletions()` 推进显式返回 Main 的 continuation；`dispatchTaskEvents()` 仅派发监控变化。
IO 运输完成和解码推进不需要先到主线程。业务采用才借用真实对象/Scene 安全点，不能把可先销毁的 Pane、
EditorContext 或可变 Registry 裸借用到 Worker。`taskResult()` 只归一化已有结果通道，不增加调度或结果 Owner。

`TaskScope::join()` 是显式同步 barrier，仅用于 Runtime/application shutdown、同步启动/API 与测试，
不得用于项目切换或 Context、Session、Pane、LuxObject 析构。Runtime 必须比 Task/TaskScope 和外部 Producer 活得久。
任务元信息有界，监控可合并；执行状态销毁后归还额度，不等待 UI 刷新。端口继续用不透明 correlation 关联父任务。
