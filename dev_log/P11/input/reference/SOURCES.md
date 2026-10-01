# 来源与施工依据

本次重新确认远端：`22ab1a30862f6bff943cef86d4232839173c7107`。

## 当前代码

下面是本包编写时核对的路径。表中的解释是对已读内容的摘要，不是完整全仓审计。新的P11/P12类型、批次和删除政策是本次设计/施工裁定，不能反写成仓库已有事实。

| 编号 | 固定源码 | 本包使用的事实 |
|---|---|---|
| S01 | [`editor/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/CMakeLists.txt) | 当前正式五层与仍存在的旧产品段 |
| S02 | [`editor/README.md`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/README.md) | 新五层、旧产品未切换的实际声明 |
| S03 | [`dev_log/P10Q-structure/README.md`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/dev_log/P10Q-structure/README.md) | 结构交付、测试范围、原工作区和用户补丁 |
| S04 | [`dev_log/P10Q-structure/retained-product.json`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/dev_log/P10Q-structure/retained-product.json) | 剩余旧provider与消费者；本次抽读前100行，非全量复算 |
| S05 | [`editor/app/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/app/CMakeLists.txt) | 当前lux_editor仍链接旧产品；正式清理不能只换目录 |
| S06 | [`editor/app/test/baseline_failures.cpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/app/test/baseline_failures.cpp) | C01/C03/C04的实际断言；尤其C04为启动连接失败 |
| S07 | [`editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp) | 旧invoke(Context&, ui::Command&) |
| S08 | [`editor/metadata/include/lux/engine/editor/metadata/EditorPluginExports.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/metadata/include/lux/engine/editor/metadata/EditorPluginExports.hpp) | V6表、符号及混合角色 |
| S09 | [`editor/metadata/src/EditorPlugin.cpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/metadata/src/EditorPlugin.cpp) | 当前通过原project loader装载并验证Editor贡献 |
| S10 | [`editor/context/include/lux/engine/editor/EditorContext.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/context/include/lux/engine/editor/EditorContext.hpp) | 旧Context/Panes/commands/assetEditors消费契约 |
| S11 | [`editor/editing/include/lux/engine/editor/sessions/SessionStore.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/editing/include/lux/engine/editor/sessions/SessionStore.hpp) | 已有reserve/prepare/publish/close；未见公开批量关闭或snapshotIds |
| S12 | [`editor/activities/persistence/include/lux/engine/editor/persistence/SaveService.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/activities/persistence/include/lux/engine/editor/persistence/SaveService.hpp) | 实际registerSource、可靠completion、采用和回调约束 |
| S13 | [`editor/workbench/desktop/include/lux/engine/editor/desktop/ViewHost.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/workbench/desktop/include/lux/engine/editor/desktop/ViewHost.hpp) | 已有adopt/drain/describeAll，不把完整布局事务视作已有 |
| S14 | [`editor/activities/project/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/activities/project/CMakeLists.txt) | editor_storage/editor_assets是正式provider，不能误删 |
| S15 | [`editor/activities/scene/CMakeLists.txt`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/activities/scene/CMakeLists.txt) | editor_editing_scene被RunStore闭包使用，保留真实运行算法 |
| S16 | [`editor/metadata/include/lux/engine/editor/metadata/ComponentEditorRegistry.hpp`](https://github.com/LUX-YU/lux-engine/blob/22ab1a30862f6bff943cef86d4232839173c7107/editor/metadata/include/lux/engine/editor/metadata/ComponentEditorRegistry.hpp) | UI factory参数仍含SceneEditing/旧InspectorInteraction，须逐语义拆分 |

## 原设计和阶段规范

`P11_original_v4.md` 和 `P12_original_v4.md` 保留原阶段的行为目标及 X11/X12；五层原设计固定依赖方向；当前总指令修正路径、原角色闭包、scope 和残留清理时间。

原资料按字节复制，仅作引用；不恢复其中已过时的顶层 commands/extensions/workflows 目录、旧账本路径、Linux硬门槛或任意旧测试数量。裁定冲突见总指令 D01–D10。

原P10Q已报告性能PARTIAL与Linux/IME未测；用户之后取消当前Linux和旧慢样本补跑的阻塞。保存历史报告不代表本轮仍需强制完成原长测。

## 证据界限

- 本次实际做了：读取原阶段规范、五层设计和代表性当前GitHub文件；编写新方案与只读盘点工具；在临时合成Git仓库测试工具。
- 本次没有做：仓库生产代码实现/删除、209项CTest重跑、插件/GPU/SDK执行、全部target图复算或P10Q完整资格认证。
- 文档中的新增API和类型是目标。没有声称现有SessionStore已支持批量原子关闭，或现有ViewHost已支持完整布局事务。
- 工具测试不是引擎测试。源码正确性、所有权、版本、容量、ABI和用户行为由实际实施后的资格验证。
