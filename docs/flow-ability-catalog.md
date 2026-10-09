# Flow Ability 描述的所有权

`ScriptAbilityNodeCatalog` 是 Flow 模块内的描述目录，不依赖 PluginManager、Editor 或 Process。
`add()` 完整验证批次后复制合同、方法、显示名称、参数、结果及其类型名称。贡献者仅需在调用期间
保持输入有效；返回后可销毁输入或卸载仅提供这些描述的 DLL。目录不执行描述提供者的代码。

目录是固定地址 owner，不能复制或按值移动。`view()` 是同步借用，在下一次成功 `add()` 或目录
析构后失效，不是可跨异步任务使用的拥有快照。拒绝批次保留原目录和已有借用。目录的平面描述
数组是其私有不可变存储的查询视图，不独立拥有或修改文本。

`validateScriptAbilityNodes()` 与目录准入使用同一校验算法，但不复制或保留输入。
FlowSource 环境验证直接调用这个无分配入口，不再为了验证而构造临时目录。

`ScriptAbilityNode` 独立复制完整描述；节点可先于目录或贡献者创建，也可晚于它们销毁。
目录和节点使用同一份私有存储构造算法，不把临时 `string_view` 或参数 `span` 留在节点中。
节点已有的 Pin/RefType、Graph 身份、编译和序列化职责保持不变。

这只处理现有 ScriptAbility 扩展面的描述寿命。它不等于完整 FlowNodeCatalog/MaterialNodeCatalog，
不改变 MA08 的 GraphTopology 唯一结构权威、开放 payload 和领域编译协议目标。
