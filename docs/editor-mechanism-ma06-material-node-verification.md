# MA06 — Material 节点目录与拥有型载荷验证

本收据仅覆盖模块公开的 Material 节点登记、不可变定义和拥有型载荷。
**MA06 尚未完成**：FlowNodeCatalog 仍待实施；现有 MaterialGraph 的内置节点、codec 和编译分派
将在 MA08 迁移。新增扩展回调生成真实 ShaderIR SSA，不代表现有图已通过目录完成编译。

实现：`d86a34b67956eaa15244b890fcb783c5dd70af8d`。lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
Windows/MSVC RelWithDebInfo，独立 clean tracked 源码、复用构建树的增量全量构建；不是冷构建。

## 所有权与边界

- GraphNodeTypeIdentity 只提供稳定 canonical name/hash/version，不拥有图或发放节点身份。
- MaterialNodeCatalog 拥有不可变定义，注册不调用工厂；完整批次验证后才发布。
- MaterialNodeType 拥有名称与 TypeToken 名称 backing，固定定义可独立于目录存活。
- MaterialNodePayload 唯一拥有语义对象，无 NodeId、PinId 或图成员指针；移动、clone 与析构保留
  CodeLease，代码 pin 覆盖对象析构及回调返回。未新增库、PluginManager 或通用管理器。
- compile 接收已解析 SSA 输入及一次性候选，返回有序多个输出；失败不授权采用候选，
  不承诺回滚回调已写入的候选。动态 pin schema 由载荷决定。

## 实际资格

| 范围 | 结果 |
|---|---|
| ValidateTrackedSnapshot | 固定实现 SHA、干净源码通过 |
| Editor all / 第二轮 / 完整 CTest | 通过 / no work / 136/136 |
| PLAYER all / 第二轮 / 完整 CTest | 通过 / no work / 72/72 |
| 全新 SDK、原消费者 | all / no work / 15/15；原名称和断言保留 |
| 最小 Material 节点 SDK | 2/2；三个公共头独立 C++20、无 RTTI 编译；真实动态库 |
| 原 Material Toolchain SDK | 2/2，四种图的八份 SPIR-V；对照 235228 字节相同 |
| 实际 source/include/link 闭包 | 679/622/57/6/4 个编译单元；模块传递依赖无 Engine、Editor、UI |
| 公共头同步 | 三个新头与 Debug、RelWithDebInfo、Android 前缀逐字节一致 |

原 Editor/PLAYER 测试各仅增加 material.node_catalog 和 material.node_plugin；原 SDK 无删除。
最小 SDK 不读取源码私有头、不链接 build DLL 或 shaderc/Toolchain。
SPIR-V 对照 SHA256：`362658d4bcb96aa1538d332886d8ee73fdc575720d6b225be5976f1a0725b6b0`。它验证原编译器未回归，独立于新增 SSA 扩展回调测试。

目录测试覆盖：原名称 backing 被销毁后读取、定义脱离目录存活、重复和批次拒绝无部分发布、
动态输出数量、错误输入/输出索引、重复 pin 语义、clone 失败保留原值、空 clone 拒绝、
移动赋值先清理旧值再释放旧 pin。碰撞测试是显式声明已有 ID 配不同名称，覆盖真实拒绝分支，
并非声称发现自然 FNV 碰撞。

DLL 测试经过真实 DynamicLibrary：销毁目录和加载方引用后仍查询 schema、生成 SSA、clone；
最终载荷析构返回后才释放最后代码 owner 并卸载 DLL。初次测试 DLL 的 C4190 导出警告原样保留，
随后改为 C 导出参数写出，不将警告记为生产失败或夸大成修复前 UAF。

## 归档与未完成范围

树外归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/node-evidence`。
1352 个文件、28 条实际命令；manifest SHA256：`6316065333203774e2b7a27ef573900f999af8d3cbd332a2805ee6a554f98262`。
中文/空格路径搬迁通过；缺失和篡改实际 SDK 测试日志均被验证器拒绝。

六处用户差异保持保护哈希；ProjectBuilder 外部补丁未应用，main 未修改。
LR08 PARTIAL、Linux 未执行/未通过、原生输入延期、IME 未测及原 minimize/WAR 判定不变。
本次没有新增 sanitizer、人工输入或跨平台成绩。Android 只同步公共头。
