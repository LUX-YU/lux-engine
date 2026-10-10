# F2 — Complete Logical Graph 工作单

批准基线 `c93f9b0ff35f556d6a95a00c585b51c21ca14b2d`；Frozen V1 `a669409a289a6fa4092f21176397795b1cdb7f3e`。
开工远端/本地一致、V2 工作区干净；719 文件及原工作区六处修改已核验。
原始保护记录：源码树外 `archives/lux-engine/RenderV2/F2/preflight-20261010/`。

## 权限

只修改 Graph、必要 bootstrap 接线及 `F2_*` 文档。Core、Transport、Vulkan、Shader Toolchain、
Description、FINAL、Legacy、历史验收只读；不创建 Native Graph、Scene/Runtime/Feature，不改产品接线。
F3-PRE-01 / F3-PRE-02 保持 OPEN。

## 实施设计与验收依据

依据 FINAL 04、06、12、13、15、16、17、19；追踪 I02、I34 及 C0–C8。

- Resource descriptor 与 access range 使用封闭 variant，消除 kind/两个 descriptor 与 whole/range 的重复状态。
- 同源生成捕获保持唯一入口；非模板验证下沉。实际 typed authoring fixture、错误 Shader/Pass 组合编译或构建拒绝。
- 编译为自由函数 `compileLogicalGraph`，只产生拥有型 `LogicalGraphPlan`；删除旧静态编译入口与旧生产调度器。
- 范围以 Buffer endpoints、Image aspect/mip/layer boundaries 分段。每段记录独立版本与初始化事实，
  不把不重叠区域串行化。writer 必须有明确 predecessor/order；read 必须具有唯一 producer，不能按声明次序选值。
- required/optional、semantic provider、fallback、import readiness/initialization 和 export roots 都是中立声明。
- 条件 producer 的输出只在已证明的同条件使用或带有效 fallback 时合法。裁剪以真实 typed output/effect 为根。
- 拓扑确定性排序、具名循环路径、版本与子范围 lifetimes、候选复用和 scope proof 均使用同一生产分析结果。
- 真实 Definition 值相等与 logical compatibility 分离；动态 scalar/sampler/clear/camera/time/backing 从 Invocation 数据读取。
- Frame borrow 收敛为明确的同步借用；临时容器与临时 Plan 有编译负例。稳定 binding 不做堆分配。
- Scene 共享必须核对中立 invocation 的 scope、相关输入/epoch/动态事实，不能用 Scene 标签直接授权共享 GPU 结果。

## 测试义务

原 99 项义务保留。旧 R3 默认 mutable 顺序的 fixture 要显式写出旧向量的版本/顺序合同，
保留原 hazard、排序、生命周期、300 随机图与 10k binding oracle，不把默认歧义重新引入生产编译器。
新增子范围 hazard、多版本、初始化缺口、producer/fallback、条件、裁剪、scope、兼容性、借用、
确定性诊断与 300–1000 组合图独立 CPU oracle。每项签名迁移记录旧/新测试映射。

## 资格纪律

完整实现后提交 I，从 I 独立 clean clone 执行 tracked snapshot、普通和完整 ASan、两轮 all -j 4 -- -k 0、
全 CTest、公共头/负例、真实依赖闭包和生成链；冷编译与稳态 binding 分开计量。
三安装头前缀同步，保护 Core/Transport/Vulkan/719 Legacy/历史证据/用户六处修改。
V 只能新增验收报告，推送后 STOP。F2 LogicalGraph 与 Native RenderGraph 资格严格分开。

本工作单记录计划与义务，不是完成或 PASS 证据。

## 用户追加授权

实施中用户确认 Transport `include/lux/engine/render/transport/Error.hpp` 的常量对齐是其修改，
明确允许保留并一并提交。仅此文件的纯空白差异作为范围例外；其它只读保护不变。
