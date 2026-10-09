# Mechanism / Authority MA00：实际基线与消费者审计

审计基线 `8d7a2a27f0ee7fcca5f824a67ad8036c110db117`，分支 `codex/editor-framework-v2`；
lux-cxx `0a0e7419fc7229df6e372cd35a540249f92250ef`。用户明确允许保留 LR08 PARTIAL 和全部未通过项，
继续 MA00–MA11，不再逐阶段等待复审。该例外不将 Linux 或历史问题改为通过。

MA00 审计通过，允许进入 MA01。本文不宣告 MA 后续实施或最终资格完成。

## 固定输入

- Editor Terminal P07 实现 `fb468d062f2e6fb39c2f8e90a55f12bb19e982db`，验收 `b2b9a6bac2f9f3e5fc70bae32271c30a7f6f85d4`。
- LR08 实现 `f15ff04175d6b6e52180903b0505771b9e32aeea`，验收即本次审计基线；二者只差验收文档。
- MSVC 19.44.35228.0 / VS 14.44.35207，C++20，Ninja，RelWithDebInfo。
- 检查 1,592 个 tracked 活动源码、头和构建输入；六处用户差异使用对应 Git 基线审计，实际工作区字节仍按原哈希保护。

## 结果与下一阶段边界

`core::services`、`core::events` 均没有实际产品源码消费者。CMake File API 的实际 Editor 376 个目标、
PLAYER 331 个目标中，services 的直接或传递消费者只有 `services_test`、`services_plugin`、
`services_plugin_test`、`service_tasks_test`，四者源码均为测试。events 没有产品依赖。
构建图来自 LR08 固定实现，已核对与本次基线之间没有生产代码差异；不冒充重新构建。

MA01 删除原提供者时，必须一并删除对应安装测试、`engine/context/test/service_tasks.cpp`、
无人使用的 ProjectOpen/SessionCreation 测试适配、SDK services 项和旧安装入口。
这些测试验证被删除的服务系统，不迁成第二套 Object 或 Editor 服务协议；EditorServices 的惰性、
递归拒绝、逆构造顺序以及项目代码寿命回归继续保留。

机制表共 24 行，分别登记权威表示、派生表示、owner、寿命、登记方式、动态代码寿命、实际消费者和阶段。
消费者数量表示排除提供者及测试后直接引用的 C++ 文件数；内部使用和传递链接另列，不能据零外部引用
判定 EditorServices 等内部能力无用。完整路径、行号和查询条件均在归档中。

活动 modules 未发现 EditorContext、LuxEngine、ProjectManifest 或 PluginManager 的产品代码引用；
实际两种构建图没有 modules 库反向依赖 engine/editor 库。生成器构建先后关系不当作运行链接。
该检查与逐领域权威审阅互补，不以字符串扫描宣称全部架构已正确。

后续严格按 MA01–MA11 顺序：先删除零消费者机制，再迁错误、RuntimeObject、日志、Render 扩展、
领域扩展、Action/Menu、图结构权威、元数据投影、全局状态分类及最终资格。
保留原 ObjectTarget/Scheduler、SceneInstanceLease、OperationPort、RenderError 和真实并发/GPU/输入协议。

## 证据与保留范围

唯一可变施工表：`.internal/editor-redesign/terminal-architecture/followups/authority/` 中的
`mechanism-authority-audit.md`、`mechanism-authority-audit.csv`；阶段快照在仓库外。
归档：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma00/`。
`audit.json` SHA256：`b230c920551838947eb0e62926b868225650c7b6a8ad5e515b8e9759a044d5b2`。

Linux 未通过；原生输入 USER_DEFERRED、IME 未测、最小化问题 OPEN、历史 skinned WAR 责任保持原判定。
不补旧长测，不修改历史快照、main 或用户补丁。后续改到受保护文件时，仅精确合并必要 API 改动，
保留用户原差异，不整文件回退或格式化覆盖。
