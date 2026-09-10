# V4-H1 实施边界与检查归属

起点 f92853ea8361a7dc90ce0da072497d3722c29f88；生产参照及镜像身份
f7d2815bdd2025ee23a7c11449def822413f58e9。两者仅有 V4 报告/归档差异。
候选分支 codex/s6-v4-prepared-checks；现有实验工作树复用其目录，内容来自 V4，未合入 A1。
独立资格 clone 及 d/t 构建槽复用，候选安装前缀 install/o/v4-h1；不覆盖 V4/A1 镜像或 SDK。

## 最终采用范围

最终资格源码为 7c565a0014d7c82f0d522eb4ca8c8f0f64773893。保留 Native H1 与 storage H3。
下文 Event H2 是 65585540 的已实施候选记录，**已从最终生产代码撤回**：七腿首次对照中 FlowForge Event
三对均变慢；唯一一次 H2 撤销对照三对改善 4.39% / 4.02% / 4.16%。保留实现提交、新增反例和原始结果，
不将 H2 写成最终已采用。最终 ScriptExecution.hpp 与 V4 完全一致。

## 实施过的生产修改（H2 后撤回）

- Native checked entry 验证当前 prepared 发布链，取得 step 引用后进入
  createNativeContinuationResolved。loadNativeModule 已检查非零大小、二幂 alignment、step callbacks；
  prepareMethod 已检查配置上限并取得 layout。创建内核仍先领取原 backend slot，再尝试 frame acquire，
  失败归还 slot；start/resume/destroy、实际 packet/outcome、模块 lease 和回收顺序保持。
  step 引用只在调用用户代码前复用，返回后的处理不以该引用代替权限重验。
- Event 的 matched source 进入 reservePreparedEventAwaitable，通用描述仍进入 reserveAwaitable，
  二者共用 reserveAwaitableValidated 动态内核。来源 preflight→A→来源 commit 不变；commit 失败
  使用已定位 record/owner 回滚，不再次按 ID 查找。来源 descriptor、scope、instance、layout epoch、
  ordinal 和 targeted entity 的边界不变；Timer 的 A→duration/source 顺序不变。
- Ticket/Allocation 分别保留外部输入验证，随后调用私有 releaseResolved。Ticket 不再合成 Allocation
  并检查自己合成的 data/size。完整 generation、owner、双重释放、统计开关和空闲链次序不变。

## Event 不能一并前移的部分

V4 的 artifact/endpoint 检查并不等价于 PreparedResumeType::valid 的全部条件。自定义 endpoint 可声明
size=6、align=4 的 struct，或非标准 ABI kind，并通过原装配；旧路径在 wait 的 source preflight 后才
返回 PAYLOAD_NOT_OWNABLE。本次真实新测试在固定 V4 DLL 上确认了这个阶段差异。

因此 H2 候选不提前冷拒绝这类输入，也不增加缓存证书/side map。已准备 struct 路径复用非零 size、二幂
alignment、owned 构造及 max_resume_payload_bytes 的已有证明，保留 type identity 与 size%alignment；
非 struct 仍使用原 valid()。这是一项有意保留的检查范围限制，不声称所有 Event 类型判断已经消除。
新增正常 struct、错 struct 布局、错误 scalar 布局、非法 kind 四个同轨迹测试，同时覆盖 source/A 双满。

## 未采用的可选项

- Native slots_per_region：匹配 FlowForge DLL 的 acquire/release 中确有整数除法。
  当前 Class 为 32B，增加 size_t 或 uint32 商都会使其成为 40B，并按 prepared_call_capacity 放大目录。
  本次保持该目录与合法配置范围，状态 NOT_SELECTED_METADATA_TRADEOFF。
- Lua：current 仅一次 prepared_abilities.at，随后填充局部 access；revalidate 使用原 execution/validity，
  不再次解析 prepared 目录。Event projection 同样只解析一次。状态 ALREADY_BOUNDARY_ONLY。
  can_reenter 对默认 scalar 的编译期选择已在 V4 存在；不记为本轮优化。closure 来源、实参、转换后
  原权限、standalone/绑定失效差异及 W0 内层 checkstack 保留。

## 验证身份

本文件不是新的测试或性能身份。最终报告分别绑定实际 clean source、DLL/PDB/EXE、原始日志和比较分布。
H0 使用冻结的 V4 EXE；它没有 ITT ROI 接口，采样包含装配/预热/oracle/清理，不冒充逐操作精确计时。
VTune 软件模式不支持 bottom-up 报表名的尝试保留；有效热点和调用链使用 hotspots/top-down。
FlowForge 目标正常结束后，collector 等待其编译工具子进程 vctip；目标 exit/oracle 已落盘后停止采集，
报表过滤到目标进程。无 PMU/cache-miss/branch-miss 归因。

保持七组件、V4 C/A/Ready/source 布局、单恢复环、业务资产、VM patch、INC、16MiB、编译器 /O2 /Ob1。
历史 scalar/Native metadata 债务未关闭；没有合并 V4/main，也不创建后续优化阶段。
