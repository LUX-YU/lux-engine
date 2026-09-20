# Process：有限异步工作与结果采用

Process 提供异步执行、资产读取和内容加载设施。它不成为所有业务 owner 的容器，也不替代 Scene、Simulation 或 Renderer 的生命周期。

## 现有结构

| 子模块 | 接点 |
| --- | --- |
| `execution` | ExecutionRuntime、TaskScope、Main 投递、计时设施 |
| `asset_loading` | 资产读取端点与 AssetLoadSender |
| `world_loading` | World 存储读取与 WorldPartitionLoadSender |

采用既有 sender／receiver 和 operation state，不另外建设轮询线程、调度器或事件总线。

## 适合交给 Process 的工作

资产读取／写入、模型解码、正式编译流程以及可以独立执行的有限计算。业务 owner 捕获明确输入，任务执行后将拥有型结果交回 owner 采用。

```text
owner 验证并捕获请求
  → sender 执行有限工作
  → set_value / set_error / set_stopped
  → Main 在正常推进点采用结果
```

“任务完成”不等于“业务已经采用”，更不等于 GPU 完成。停止结果不能当作成功结果，失败之前已经发生的文件效果也不能因错误返回而被抹掉。

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

拒绝接纳保留调用方输入；接纳后 operation state、输入资源和代码有效期由明确 owner 保持。取消意图不是任务已停止的事实。

关闭期间继续推进必要完成，直到已接纳工作获得终态并解除借用。不能用强制结束进程或空日志代替正常收尾。

本文不新增 OOM 恢复工程；正常 IO、解码、编译和业务错误仍须准确保留。

相关说明：[World](../domain/world/README.md)、[Simulation](../domain/simulation/README.md)、[Scene Editor](../editor/editors/scene/README.md)。
