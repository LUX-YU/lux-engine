# L5：在真实复用点使用 C++20 concept，动态边界只保留一次

前置：L1–L4 的新 owner 和依赖已清楚。本批不以新增概念数量作为成果。详细代码契约见 `03_CONCEPTS_AND_DYNAMIC_BINDING.md`。

## 1. 必做：约束现有 InteractionDelivery

原共享算法存在于 P10Q，实际被 MaterialView/FlowView 使用。L3 将其归 workbench 后，L5 完成：

1. 定义两个小的内部概念：`VoidDeliveryResult<R>` 与 `DeliveryAction<F,R>`，只要求算法真实用到的构造/调用/Result 能力。
2. 以 Validate 的 Result 类型为共同结果，对 cancel/begin/preview/finish 返回完全相同类型作约束。
3. 函数仍只持当次栈上的 actions，保留原 stage 参照；不拥有 queue/payload/stamp。
4. 原 `EInputDeliveryStage` 值保持，成功才推进阶段；失败保持原阶段和错误。
5. 不给算法增加泛化 DomainKey、TaskId 或全局调度功能。
6. 不能在共同 helper 把错误变成 bool，或一律映射 BUSY；两个领域沿原准确错误返回。
7. 两个工具都使用这一份函数定义，不能一边迁入新 concept 版一边保留旧裸模板旁路。

## 2. 必做：抽象清单与动态边界审查

每个现存 Port/Provider/Controller/Operation/大模板列清：
- 是否有独立事实/不变量/寿命；
- 运行时是否允许未知实现；
- 实际静态实例化者；
- 是否只是转发同一对象的方法；
- 是否把两种错误/结果混成一个 action。

结论只允许 KEEP_CONCRETE、KEEP_DYNAMIC、CONSTRAIN_STATIC、MERGE_FUNCTION、DELETE_FORWARDER、DEFER_P11/P12（必须已有旧消费者）。不能全部 KEEP 且不解释。

明确保留：IEditSession 的异构 Store，ISaveSource 的运行角色，适当的 IEncodeJob/IPreparedRebind，运行时未知 factory 的一次边界。它们不是为了测试而模拟出的多态。

## 3. 条件做：FrozenEncoder

若现有三类编码 job 只有同一控制骨架、差异仅为 snapshot/codec，并且至少两种真实编码可共用，则将共同核约束为 FrozenEncoder，在真实 job 构造/注册处实例化一次，再由既有 OwnedEncodeJob 擦除。

不满足条件就不做，记录具体差异，例如 Scene 多文件快照包、Material 图编码、Flow 冻结值具有不同输出和预算语义。不要为了“concept 必须用两次”添加对称 Codec 包装类或新 Result。

原接口已足够正确时，使用一个普通 concrete API 是抽象，不必补 `ISceneSession` 或 `TSaveService<...>`。

## 4. 继承、组合和 variant

- Pane/Element 的继承体现 UI 节点可替代性；LuxObject 体现真实活动对象的线程/信号，不为所有纯值加入对象基类。
- Session 与 Source/History/State 使用组合；不存在 SceneSession→MaterialSession 的继承。
- View 必需服务用引用/不可空构造；真正 Unbound/Author/Running 用现有 variant 表达。
- 控制 owner 的特殊成员显式；领域结果可以复制，控制责任不能顺便复制。
- 无状态函数不要包 Operation 类；已有 Q10 删除的 Publish*Operation 不得复活。
- 不为减少 void* 把低层合法 ABI/类型擦除机制改成多层 std::function；只整改普通业务中没有必要的擦除。

## 5. 运行时语义不能交给 concept“证明”

编译器约束可以拒绝错误返回类型/调用形式；不能证明 payload 深层拥有、预算真实、schema callback 不重入、内容身份仍有效、代码 owner 尚存、文件没有被别的进程修改。

因此保留：factory 构造不变量；跨回调/异步/下一帧/注册撤销的重验；文件最终发布前冲突检查；Runtime/Host 保护；已接纳完成事实的可靠接收。

删除冗余检查必须记录首次证明点、使用范围、失效事件、实际消除的工作和对应回归。不要做“if 数量减少”统计作为目标。

## 6. 概念资格

在当前已验证 C++20 编译器上：
- 两种真实 View 的实际 actions 是正例；
- 一个 action 返回 bool、另一 action 返回不同领域 Result、错误 cv/ref 可调用、Result 不满足 void 语义均为对应负例；
- 负例必须先能 include 当前相关真实头，不可把缺头/缺第三方库当 concept 拒绝；
- concept 放内部 sinclude 时，源码契约测试使用精确内部授权；SDK 消费者仍通过公开 MaterialView/FlowView 验证真实实例化，不强行把私有 helper 安装出去；
- 不使用本包独立说明性片段作为 Lux SDK 的运行证明。

## 7. L5 出口

共享算法只有一份、有两个实际使用点、明确语义律、正反编译证据。动态抽象没有被重复包装；模板没有出现在 application 的全服务树上。所有删除的转发入口都有真实消费者迁移记录。
