# MA06/MA08：Flow 来源适配职责闭合

实现：`9de3a098cb71edcbdecdf21e49422477f35b2cb6`，起始实现 `1cfe64d24`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
**本次来源适配闭环 PASS；MA08 实际图编辑器渲染仍未资格化，整个机制整改未完成。**

## 唯一职责与删除

`FlowNodeRegistration` 的 `capture_source` / `restore_source` 成为内置及外部节点共同的来源入口。
Scalar、Control、Function、Object、Script Ability/Event 提供者分别拥有自己的参数捕获和元信息解析。
公共纯值集中在 `FlowSourceData.hpp`；共同的类型/类/参数查找只有一份私有实现。
删除旧 `encode` / `decode` 字节回调、`FlowNodeType` 同名入口，以及 `FlowSourceGraph` 的内置种类分支和具体载荷解析体。
实际 DLL、编译器、SDK 消费者均迁至新合同，没有转发壳或双算法。

图恢复仍只构造一个未发布候选，最后经原 FlowGraphEdit 一次提交。
声明阶段不暴露部分声明；正文阶段只借用完整声明集合及变量，不能观察先恢复的正文节点。
该规则避免 source 排列顺序成为隐含协议，不是第二套图依赖调度器。
真实外部注册测试覆盖两个声明、两个正文、前向引用、变量查询、元信息缺失、准确失败及版本拒绝前不调用扩展。
失败后完整来源与原图保持一致。借用查询不得跨同步调用保存。

冻结 v1/v2 编码、图身份、pin 顺序/default/layout、唯一 GraphEdit、编译算法和脚本元信息保活均未改。
原 33 份真实旧 SDK v1 输入仍逐份恢复并产出字节一致的 v2。

## 最终实现验证

独立 clean tracked 源码检出；Editor/PLAYER 使用既有增量构建树，**不称为冷构建**。
两者全量 `all -j 4 -- -k 0`，第二轮无新增工作；Editor **171/171**、PLAYER **97/97**。
与 MA09 最终矩阵的原测试名称逐项一致，未删危险断言。

全新 SDK：`D:/LuxQualification/ma08-source-adapters-r2-install`。
六组消费者 analysis/native/scalar/graph/payload/ability 均全量构建、无新增工作、执行通过。
实际数量依次为 4、7、7、4、2、3；数量只用于标识矩阵，行为证据另核对。

- 新旧 source 往返、动态 pin、真实 DLL 载荷/代码寿命、三组各 77 次注册多输出原生调用保留。
- 123 scalar 导出（28,393 字节）、async（4,931 字节）、control（5,820 字节含 framing，5,772 字节对象）与原始基线逐字节一致。
- 原 module/compiler 两条路径的 12 行失败诊断逐字节一致；48 次嵌套控制原生执行通过。
- 实际读/写测试仍观察 1→7，再次调用为 7。
- 独立 SDK 正例先通过；四项旧字段/旧方法负例均由真实公共头报 C2039；每次移除非法调用后恢复构建和执行。
- 编译命令不借用源码生产私有头或旧构建 DLL。Flow 的真实提供者闭包仅 description/flowforge/graph/meta/object/script_core。
- 三个改动公共头在新 SDK 及 Debug/RelWithDebInfo/Android include 前缀逐字节核对；Android 仅同步头。

首次开发构建遗漏实际 DLL consumer、测试误把 bool 当 expected，以及补充 SDK 夹具的 Windows 路径转义失败均保留。
资格脚本一次错误复用已有消费者目录时被保护检查拒绝，未覆盖原目录；后续使用独立 r1/r2 前缀。
这些是实际首次结果，不改判、不以草图代替运行。

## 证据与保留范围

归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/source-adapters/evidence`。
325 个文件、147 条命令，manifest SHA256：
`28350524bce87752e78956371754e40a1e2b8bcdd4bc4821205a7e3a28c183d2`。
中文/空格路径搬迁、真实日志缺失及篡改拒绝已执行通过。各历史运行保留原 implementation SHA。

六处用户差异哈希不变、未提交；ProjectBuilder 历史补丁仍未应用。main 与历史快照未修改。
此前 `render.transfer_idle` 缺少完成标记的失败仍未解释；本次通过不构成根因修复。
实际图编辑器渲染、MA10 最终审计和 MA11 总资格继续推进；普通 UI/GPU 成绩不冒充图编辑器资格。
Linux 未提供环境；原生输入 USER_DEFERRED、IME、sanitizer、历史性能及其余原未通过范围保持原记录。
