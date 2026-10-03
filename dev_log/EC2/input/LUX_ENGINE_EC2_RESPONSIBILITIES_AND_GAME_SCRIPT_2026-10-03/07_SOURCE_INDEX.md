# 07　固定来源、事实与执行边界

## 1. 本次实际做了什么

核对当前 Git ref/commit，读取 EC1 报告、上一轮职责调查和 AGENTS，并读取下列代表性头/实现。没有 clone/编译整个项目，没有运行 EC1/ScriptSystem/Lua/GPU/PLAYER 或完整归档验证。也没有根据无法检索的巨大树推断某模块不存在。

本包是施工设计，不是 EC1 PASS 收据。新类型/API/叶子位置均为目标设计。凡只是旧调查的发现而没有本轮重读完整实现者，R0 必须复核当前消费者和剩余责任。

固定代码 HEAD：`248adc4576943cab83976afd8d1d5f31b63b70a9`。源码引用为固定 GitHub blob URL，不随分支以后移动。API ref URL 只用于本次定位，不作为未来固定代码。

## 2. 当前代码与报告

| ID | 来源 | 本次读取与用途 |
|---|---|---|
| C01 | [分支 ref](https://api.github.com/repos/LUX-YU/lux-engine/git/ref/heads/codex/editor-redesign-v4) | 2026-10-03 返回 248adc4576943cab83976afd8d1d5f31b63b70a9 |
| C02 | [验收提交与实现父提交](https://api.github.com/repos/LUX-YU/lux-engine/git/commits/248adc4576943cab83976afd8d1d5f31b63b70a9) | parent 137b8f441faa1dbb7dfa792b6f01b513327e6248 |
| C03 | [dev_log/EC1/report.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/dev_log/EC1/report.md) | 完整报告；实施方证据摘要，不是本次独立运行 |
| C04 | [editor/application/src/EditorArtifacts.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/application/src/EditorArtifacts.cpp) | 1–155；新 DerivedArtifact 输入及仍在 Application 的执行段 |
| C05 | [editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp) | 完整头；固定字段与 owner_ 混合仍在 |
| C06 | [editor/activities/material/include/lux/engine/editor/material/MaterialPreviewStore.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/material/include/lux/engine/editor/material/MaterialPreviewStore.hpp) | 完整头；任务输入和 live preview 接口 |
| C07 | [editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp) | 完整头；控件、正式配置值和 builders 依赖 |
| C08 | [editor/workbench/scene/src/SceneConfigurationElement.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/workbench/scene/src/SceneConfigurationElement.cpp) | 540–725；当前 applicability、preset、systemOptions 起点 |
| C09 | [editor/application/src/EditorResults.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/application/src/EditorResults.cpp) | 105–185；VResultIntent 已使用，ResultsPane 仍借 AppImpl |
| C10 | [editor/activities/material/include/lux/engine/editor/material/MaterialCompilation.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/material/include/lux/engine/editor/material/MaterialCompilation.hpp) | 完整头；key.target、CompiledMaterial 与 preview error 域 |
| C11 | [editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceStore.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/editor/activities/workspace/include/lux/engine/editor/workspace/WorkspaceStore.hpp) | 完整头；query/IO/policy/migration 接口 |
| C12 | [modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp) | 1–250；typed/erased completion、方法、值寿命；不是全文件穷尽阅读 |
| C13 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp) | 完整头；外部结果类型限制与原 awaitable/start 流程 |
| C14 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptLocalAsync.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptLocalAsync.hpp) | 完整头；ScriptTimers 私有资格 |
| C15 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptApiCapability.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptApiCapability.hpp) | 完整头；publication/prepared capability 形状 |
| C16 | [engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp) | 完整头；config/values/abilities/stats |
| C17 | [modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp) | 完整头；value operation 与 generated traits 入口 |
| C18 | [engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp) | 完整主要头 1–260；read sender/typed decode/cancel |
| C19 | [engine/process/asset_loading/include/lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/include/lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp) | 完整头；请求保活/TaskScope/stop/join |
| C20 | [engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/ScriptSystem.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/ScriptSystem.hpp) | 1–355；runtime budget/stats/create/region/lifecycle |
| C21 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/DeferredScriptHost.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/DeferredScriptHost.hpp) | 完整头；显式 component 绑定与命令写入区域 |
| C22 | [engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/abilities/DelayAbility.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/include/lux/engine/simulation/abilities/DelayAbility.hpp) | 完整头；实际注解式能力声明 |
| C23 | [engine/domain/simulation/builtin_systems/script/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/builtin_systems/script/CMakeLists.txt) | 完整；lux_script_abilities 真实生成与安装 |
| C24 | [engine/process/README.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/README.md) | 完整；Process 职责；旧编辑器链接文字不作为当前目录证据 |
| C25 | [dev_log/EC1/S2.md](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/dev_log/EC1/S2.md) | 完整；Editor V8 三类能力组合的实施方说明 |
| C26 | [modules/function/script/lua/include/lux/engine/function/script/lua/LuaBoundary.h](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/modules/function/script/lua/include/lux/engine/function/script/lua/LuaBoundary.h) | 完整；typed worker 正常析构后 C error/suspend |
| C27 | [engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp) | 1–170；读取数量准入及 vfs.open 调用；未审完下游 provider 内存边界 |
| C28 | [engine/domain/simulation/scripting/lua/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/lua/CMakeLists.txt) | 完整；native/Lua/runtime 的实际依赖 |
| C29 | [engine/domain/simulation/scripting/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/CMakeLists.txt) | 完整；backend 分组与当前非 Android Lua 条件 |
| C30 | [engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp](https://github.com/LUX-YU/lux-engine/blob/248adc4576943cab83976afd8d1d5f31b63b70a9/engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptEndpointBridge.hpp) | 1–230；Hook/Event 描述与回调/值复制协议 |

## 3. 用户材料与外部背景

| ID | 来源 | 用法 |
|---|---|---|
| U01 | [用户 AGENTS 原件](references/AGENTS.user.md)；当前仓库根 AGENTS 同一 LF Git blob `1fd3123156dc7f59f98cbcd422f227a16352cddc` | 风格、依赖、异常、observer、构建/同步。复制原字节，不因本包意见修改。 |
| U02 | [上次职责调查原件](references/PRIOR_RESPONSIBILITY_AUDIT_54f8a563.md) | 旧基线 `54f8a563…` 的调查。EC1 已改事项由 C03 和新源码纠正；“并入 EC1”不再执行。 |
| W01 | [Lua 5.5 Reference Manual](https://www.lua.org/manual/5.5/manual.html#4.4)，§4.4、§4.5、userdata 相关定义 | 只用于解释非局部错误/yield 和 continuation；实际绑定仍以本仓库 Lua55 和 LuaBoundary 为准，不据此升级依赖。 |

## 4. 证据分级

**当前源码事实**：例如 EditorArtifacts 已用 DerivedArtifact、Preview 仍用 CompileOperation、外部 async 结果限制、LuaBoundary 合同。

**实施方报告事实**：EC1 测试数量、骨骼模式、user patch/工作树、性能样本和删除结果。它们是报告内容，本次未独立重跑。

**设计裁定**：EC2 的职责目标、API/作用域/目录建议、测试矩阵。不能把这些未来要求说成已经完成。

**待执行核对**：全部实际消费者、逐实例 capability 的完整内部链、VFS provider 的读取前字节限制、完整安装/target 闭包。提供者已有等价实现时复用，不重复建设。

## 5. 文档本地检查不是引擎资格

本包只进行了文件生成、链接/引用/代码围栏/JSON/压缩包校验、用户参考字节比对。没有生成伪运行结果。`PACKAGE_CHECKS.json` 描述这些本地文件检查，不进入工程 PASS 计数。
