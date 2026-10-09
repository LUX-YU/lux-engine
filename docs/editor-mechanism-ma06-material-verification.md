# MA06 — Material 编译合同的模块归属

本收据仅覆盖 Material 的纯编译合同与唯一指纹算法迁移。**MA06 尚未完成**：
MaterialNodeCatalog、FlowNodeCatalog、开放 payload/编译回调及 MA08 图结构权威仍待实施。

实现：`4e783f2984a2e5be320756ca96d10427389c3931`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`，沿用现有源码和安装依赖版本。
Windows/MSVC RelWithDebInfo；独立 clean tracked 源码、复用构建树的增量全量构建，不是冷构建。

## 实际迁移

`ShaderIR.hpp` 和唯一 `computeFingerprint()` 实现由 Toolchain 私有目录迁入现有 `material_graph`。
现有 MaterialLowering、GLSL/SPIR-V 后端直接使用这一份合同；shaderc、反射及编译执行仍由
`toolchain_material_compiler` 提供。未新增库、转发头、PluginManager 依赖或第二份指纹算法。

`MaterialCompileFailure` 从 Compiler.hpp 迁为模块公开的纯领域错误。错误码、NodeId、pin index
及拥有型文本保持；删除没有可恢复生产者的 ALLOCATION_FAILURE，普通堆 OOM 仍 fatal。
原 graph validation、两个实际 shader pass 与 foreign containment 未重写。

## 验证与对照

| 范围 | 实际结果 |
|---|---|
| ValidateTrackedSnapshot | 上述实现 SHA、干净源码通过 |
| Editor all、第二轮、完整 CTest | 通过、no work、134/134；原 132 项保留，增加两项 |
| PLAYER all、第二轮、完整 CTest | 通过、no work、70/70；增加纯指纹测试，不引入 Toolchain |
| 全新 SDK、原消费者 | all/no work、15/15，原测试名称无删除 |
| 仅 Material 模块的安装消费者 | 1/1；两个公共头独立 C++20、无 RTTI 编译 |
| 实际 Toolchain 安装消费者 | 2/2，四种图、八份 SPIR-V 和准确图错误 |
| 指纹算法 | 原实现导出的 16 个指纹与新公开实现逐项相同 |
| 实际编译产物 | 旧 SDK 与新 SDK 的对照文件（含长度前缀）共 235228 字节完全一致 |
| 真实依赖/安装闭包 | 674/617/57/3/4 个编译单元；material_graph 传递依赖无 Engine/Editor/UI，最小消费者无 shaderc/Toolchain |
| 三个 include 前缀 | 两个 modules 公共头逐字节一致；Android 仅同步头 |

SPIR-V 对照文件 SHA256：`362658d4bcb96aa1538d332886d8ee73fdc575720d6b225be5976f1a0725b6b0`。
旧 SDK 使用上一 Ability 实现的全新安装；原私有 ShaderIR 没有旧公开 SDK 入口，故指纹基线
直接编译 `d1e6a5d877f58e0553933f4118c9ecf4602ae1cf` 中的原始头/源，并分别保存其 Git 来源。
该指纹基线不冒充已安装 SDK 消费者。新消费者仅使用安装公开头和库。

第一次新增编译夹具错误地期待空图返回 MISSING_REQUIRED_OUTPUT，实际原合同为 INVALID_GRAPH。
首次失败及原夹具保留；修正夹具后另增非空无输出图来验证 MISSING_REQUIRED_OUTPUT。
未改变生产错误分类，也未以夹具错误宣称发现生产缺陷。

## 证据与保留范围

归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/material-evidence`。
1360 个文件、35 条命令；manifest SHA256：`25e9f585168ac7e852289439ef6b0decee1069ca07532d344fdc0f05ddb84e2b`。
搬迁至中文/空格路径完整校验通过；缺失或篡改实际安装测试日志均被拒绝。
原始输出、对照 SPIR-V、源文件和失败记录均在树外归档，不改写历史收据。

六处用户修改保持原保护哈希，ProjectBuilder 外部补丁未应用。main 未修改。
LR08 PARTIAL、Linux 未执行/未通过、原生输入延期、IME 未测及既有 minimize/WAR 判定保持。
本次不增加 sanitizer、人工原生输入或跨平台资格，不以旧 SHA 成绩冒充重跑。
