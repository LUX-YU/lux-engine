# Flow 节点语义载荷与编译错误

`FlowNodePayload` 属于现有 `flowforge` 模块。它只拥有语义数据及执行其析构、clone 所需的代码，
不保存 NodeId、PinId、图成员指针或链接。GraphTopology 仍拥有结构事实。
它不继承旧 Node，也不成为另一个图或执行器。

类型化 `make<T, Clone>()` 要求无异常构造、析构和返回 `FlowForgeResult<unique_ptr<T>>` 的
无异常 clone。普通堆 OOM 仍为 fatal。空 code lease 在构造前拒绝，失败 clone 不改变原载荷；
“成功但返回空值”的 clone 被明确拒绝。`get<T>()` 仅提供同步类型化借用，不使用 RTTI。
借用或 clone 执行期间，调用方必须保持原载荷，不能并发替换或销毁它。

移动赋值先清理旧载荷，之后才释放其代码 pin。模块的析构实现把 CodeLease 保持在局部拥有单元中，
覆盖插件析构、deleter 及返回过程。复制只能显式调用 fallible clone，不隐式共享可变语义对象。
代码寿命沿用 Object 模块的 CodeLease，不依赖 PluginManager 或 LuxObject 线程域。

`FlowForgeFailure` 的唯一声明从 Toolchain 公开头迁入本模块，仍包含原错误类别、拥有型消息及
节点/引脚诊断身份。现有编译器直接使用这一份声明，不留下同义别名或转发文件。
没有生产者的 ALLOCATION_FAILURE 删除，其余既有枚举数值保持；完整 MLIR/LLVM 降低和 AOT
算法仍留在 Toolchain。

这还不是完整的开放 Flow 图。FlowNodeCatalog 的 compile 接点必须接通真实的控制流、数据流和
ScriptAbility 编译语义，不能公开私有 MLIR 类型、返回默认成功或包装旧 Node creator。
现有 FlowGraph/FlowSource 和编译器尚未迁移到此载荷，后续按 MA06/MA08 完成，不能以载荷测试
声称开放图或内置节点迁移已经通过。
