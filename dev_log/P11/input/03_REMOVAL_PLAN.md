# 旧代码、目录、构建与安装残留的实际移除计划

这不是可以直接执行的批量 rm 清单。实施者必须为每项找到真实消费者，完成替代并运行对应行为，再删除原实现。完成删除意味着源、依赖和运行入口同时退出，不是文件换个目录。

## 1. 四类动作

| 动作 | 判据 | 出口 |
|---|---|---|
| KEEP_FORMAL | 已是五层中的真实正式 provider | 保持其语义/行为，可修正不准确的文档，不能按旧名字误删。 |
| EXTRACT_AND_DELETE | 旧文件中仍有唯一有用算法/配置/控件 | 迁到唯一正式 owner，消费者切换，删除原体和原导出。 |
| REPLACE_AND_DELETE | 旧 owner/协议已被新系统替代 | 实際消费者改用正式路径，原声明/定义/目标/包退出。 |
| RETAIN_DATA_OR_HISTORY | 数据读取兼容、用户备份、历史证据 | 保留并隔离说明，不被活动产品编译成旧系统。 |

不新增“先移 legacy，以后再说”第五类。

## 2. 九个旧根目录的最终处置

| ID | 当前根 | P11 处置 | P12 必须完成 | 删除前的等价证明 |
|---|---|---|---|---|
| R01 | `editor/metadata` | 拆分纯值/控件/角色/出口；正式新链不使用旧总表；删除已迁原体 | 剩余旧工厂与可变注册协议退出，旧 metadata aggregate target/包删除，目录清空 | 组件/配置/命令/Session/View 插件实际可用，代码寿命正确 |
| R02 | `editor/plugins` | 迁 Editor 贡献与生成输入到 application/extensions 或对应 workbench provider | 不再通过旧 exports/metadata 注册；原路径、生成 CMake 和多余 wrappers 删除 | 内置 scene/render/physics 等当前支持扩展逐项注册、配置和构造 |
| R03 | `editor/context` | 新链禁入；旧 query 的 C03 安全责任不得忽略 | 删除 EditorContext、PaneManager、私有 ContextAccess、旧共享 app owner | 新 App 服务组合、内容打开、工具复用、退出完整工作 |
| R04 | `editor/ui` | 已抽唯一算法直接共享；剩余控件/策略确定新归属 | 删除旧 UI 聚合、旧 InspectorInteraction/Spatial 兼容入口及生成支持 | 原控件/配置/任务/viewport 适用功能迁至正式 UI并实测 |
| R05 | `editor/tools` | 仅旧工具；新厂/新视图不得经它们获得业务能力 | 删除旧 SceneEditor/MaterialEditor/FlowForgeEditor、Impl、friends/TestAccess；settings/project旧壳迁完删 | 三工具正式功能矩阵+真实产品操作+旧行为等价测试 |
| R06 | `editor/launcher` | 现有 launchEditor 已共享；不提前复制项目创建 UI | 唯一真实 UI 归 workbench，main/进程接线归 application/launch；旧根删除 | 无项目启动、创建项目、打开项目、启动正式 Editor |
| R07 | `editor/app` | 旧 executable 仍工作；新命令/贡献以正式机制验证 | 当前 lux_editor target移至application；旧 Editor.cpp/Startup/Loop/Menu/Workspace/Platform/Impl/TestAccess 删除 | 安装后的真实 exe 启动/菜单/布局/退出；C01/C04通过 |
| R08 | `editor/assets` | 当前剩旧保存适配测试，不是新 AssetImporter provider | 有用测试迁到正式测试位置，旧 TAssetSave/SceneSave 等私有输出与CMake退出 | 正常保存/失败/重试/组合产物的原用户行为不丢 |
| R09 | `editor/transition` | 禁止新增；已无消费者的桥立即删 | 所有桥、LegacyPersistenceState、编译输入/配置/生成适配全部删，白名单为空 | 每项真实消费者的替代调用、构建和行为证据 |

P11 仍需要旧产品头的情形必须逐文件记录，不自动以这张根表作为所有内容的延期授权。P12 结束上表九个根都不应再出现在 `git ls-tree HEAD:editor`。

## 3. 正式目录内部的旧残留也必须处理

| ID | 位置/符号族 | 处理 |
|---|---|---|
| R10 | `editor/editing` 内 old `editor_editing` target、EditHistoryTarget 与旧混合资产/关闭协议 | 逐消费者迁移后删除旧 target/源；不要删除同目录的 edit_history/edit_sessions/editor_contracts。 |
| R11 | `AssetEditing.hpp / AssetOpenRequest.hpp / AssetSave.hpp / CloseRequest.hpp / CloseStatus.hpp` 等旧业务入口 | 替换为具体 Open/Save/Close 和明确 Result；删除已无消费者声明，不在原路径留 forwarding header。 |
| R12 | `activities/project/sinclude`、`activities/sinclude` 中仅旧产品使用的 `TAssetSave` 或 SaveRequest 类型 | 根据真实调用图删除旧算法/注册。Shared Result/Signal helpers 若仍为正式调用所需，不按所在旧逻辑 include 误删。 |
| R13 | 旧 `SceneSaveCapture::copied`、旧 SceneSave/MaterialSave/FlowSave、CompiledAsset 重编码 helpers | 保留新 SceneSaveSource/MaterialSaveSource/FlowSaveSource 与 Codec；只清旧接口和重复输出通道。 |
| R14 | 旧 `setCommands/setRegistrations → span`、`invoke(EditorContext&, ui::Command&)` | 正式当前条目使用不可变 snapshot + handle；当前回调不依赖可变集合。最后旧消费者退出后删全部原接口。 |
| R15 | 旧 PaneRegistration/AssetEditorRegistration/CommandRegistration | 新角色和 factory 完成后删除旧可执行 C++ 合同；数据中旧 type 字符串可由版本化导入器读取。 |
| R16 | 顶层 Root 自动注册构造、旧 `root()` 假设 | 删除精确兼容签名及调用，保留离树子节点组合、合法挂载访问和底层唯一 owner。 |
| R17 | 旧 TestAccess、friend、baseline-only 生产写入口 | 测试转为正式能力或窄故障点；不在安装 SDK 开放任意修改内部状态。 |
| R18 | 旧名字的新壳、INTERFACE 聚合 alias、旧 find_package 回落 | 只有真实实现才能保留 target 名；只有转发到新包的旧壳一律移除。 |

匹配符号族是调查提示，不是精确 AST 删除命令。`SceneSaveSource` 包含 SceneSave 字样但不是旧 SceneSave；`LegacyWorkspaceImporter` 是需要保留的数据导入；不能以子串替换破坏它们。

## 4. 明确的保留清单

| 对象 | 为什么保留 |
|---|---|
| `editor/activities/project` 的 editor_storage / editor_assets | CMake 中分别有真实 ProjectStorage 和 AssetImporter 源，不是空兼容 target。 |
| `editor/activities/scene` 的 editor_editing_scene | 真实 SceneEditing/ComponentEdit 被 RunStore 消费；清理旧窗口不等于删除运行编辑。 |
| `editor/application/launch` 的 editor_launch | 已有唯一进程启动实现；复用它而非重写平台启动。 |
| 纯 ProjectBuilder/ProjectBuildConfig | 只构造和验证配置，不是旧编译工作流。 |
| LegacyWorkspaceImporter/原 codec/旧格式只读数据 | 保留用户数据可读性；不允许它们成为重新打开旧业务系统的入口。 |
| 原 World 分区纯解码与 Process 复用 | 本次结构整改已经消除反向依赖，不能再将纯解码拉回 Process。 |
| CodeLease/History/Session/Runtime 的共享状态边界 | 不通过一键 STATIC 在多个 DLL 中复制“唯一”身份或注册状态。 |
| 历史 dev_log、原提交、原 FAIL/NOT_RUN 证据 | 删除当前生产实现不删除可复核历史。 |
| 用户工作区、ProjectBuilder 用户差异、项目/资产/布局备份 | 不是本阶段施工垃圾。 |

## 5. metadata 的逐职责处理

不要把整个 metadata 目录改名为 extensions 后保留原依赖。

| 内容 | 目标位置 | 注意 |
|---|---|---|
| 纯字段/配置描述、PortableValueCodec、配置值 | authoring 对应域或已有纯基础定义 | 不让 UI factory 通过 PUBLIC 传播至纯作者模型。 |
| ComponentEditorRegistration 的 UI create | workbench/scene | 当前 create 仍依赖 SceneEditing 和旧 InspectorInteraction，不是纯 schema；改为正式作者字段能力/明确运行能力，保留两种语义差别。 |
| ComponentEditorRegistry 真正不可变目录算法 | workbench 合理 provider | 名字有实际语义可保留，原 aggregate target 和旧函数签名不保留。 |
| Command/Session/View 可执行 factory | 各自 activities/workbench | 不能组合成 E0 的万能服务表。 |
| EditorPluginExports/LoadEditorPlugin 装载与安装 | application/extensions | 复用 engine/project loader，显式新出口版本。 |
| 内置生成注册函数 | 实际贡献所在 target | 查清 generator->consumer，不能按 consumers=[] 将有生成职责的目标当作死代码。 |

## 6. 每项删除必须完成的九步

1. **定位：** 确认当前 ref 中实际文件、blob、声明/定义、CMake provider 和直接/传递消费者。
2. **分类：** 标记替代、提取、正式保留或历史保留，写明理由。
3. **替代：** 完成对应正式能力，不新增空默认成功实现。
4. **接线：** 修改实际产品、内置贡献、测试、示例、SDK 调用与生成输入。
5. **回归：** 运行同义行为，原结果/身份/失败可观察性不丢。
6. **删除：** 删除原声明、定义、转发、source list、安装/export 和无引用生成物，不只注释/改后缀。
7. **闭包：** 检查真实 source/depfile/CMake File API/最终链接，确认没有静态 LINK_ONLY 暗藏旧库。
8. **安装：** 新前缀独立消费；开发前缀仅按既有安装清单删除已确认废弃文件。
9. **证据：** 记录替代提交、删除提交、验证命令和旧/新测试映射，清空相应暂留项。

删除不使用整目录无差别 rm，不对用户安装前缀使用无来源递归清除。自动化脚本只能删除经过人工审核、在受控安装清单中且路径规范化后位于指定前缀内的条目；仓库生产删除正常用明确 `git rm`。

## 7. CMake 与安装残留

- root 删除九个旧 `add_subdirectory`；五层内部不再引用旧 source roots/pinclude/sinclude。
- 删除旧 editor_context/editor_ui/editor_app/三大 Editor aggregate 的 source target、导出宏和 alias。不能改成无源 INTERFACE 继续发布旧名字。
- 正式新 provider 逻辑包名正确且有真实源码时可以保留，不能因它曾来自旧路径就全部更名。
- 清理旧生成器的 installed cmake scripts、类型扫描输入、生成入口函数和 target dependencies；唯一新生成器继续工作。
- SDK 不安装私有 headers、TestAccess、施工账本、历史日志或旧 API。
- 安装内置扩展的新版库、符号、配置路径与 runtime/editor版本声明；原 runtime 插件闭包保持独立。
- Windows 检查实际链接/装载清单，旧 build DLL 不能被 PATH 悄悄找到。无需本阶段新增 Linux 动态装载实测，但其平台分支不得硬编码 Windows。
- 同一公共 logical include 只由一个 provider 安装；共享 build include root 不代表可重复 install 整个目录。

## 8. 当前与历史测试的区别

原 `baseline_failures.cpp` 使用旧 Editor/PaneManager，是历史失败输入。P11/P12 建立相同语义的正式后继测试，通过后把旧测试源作为历史引用而非当前旧产品构建依赖。

必须保留三个契约：

- C01：拒绝坏布局时，原可见性、数量和 dock 不变化；
- C03：query 自替换时，旧闭包/代码到回调返回后才释放；
- C04：菜单连接失败使 create 失败，不发布成功应用。

不要用 WILL_FAIL 反转运行期缺陷断言。新契约通过与历史原实现失败可以同时存在，不改写过去报告。

同理，旧协议测试的 name 可保留或有映射地改名；测试数字不决定质量。不得在新 `tests/legacy` 中链接一个完整旧 Editor 来维持“原测试未删除”。

## 9. 最终清零验收范围

必须同时检查：

| 维度 | 合格结果 |
|---|---|
| Git 一级目录 | 五层 + tests；无旧九根。 |
| 活动源定义 | 无旧 owner/协议及同义改名替身；无 .old/.bak/#if0 缓存生产实现。 |
| 头与 include | 无原根转发壳、私有反向 include、旧安装补依赖。 |
| CMake/生成 | 无旧构建入口、alias 或长期 EXCLUDE_FROM_ALL 旧系统；生成器实际可重建。 |
| 产品运行 | 安装的 lux_editor 走唯一新装配；没有 old/new/fallback 启动模式。 |
| 动态装载 | 新扩展版本明确，旧 Editor 出口不再参与生产；共享身份不重复。 |
| SDK | 真实独立消费者运行，旧包不存在或明确在不兼容负例中被拒绝。 |
| 用户功能 | 原适用用例都有实际 UI/命令入口与结果，非仅模型函数能调用。 |
| 保全 | 用户变更、原 codec 数据、历史收据和未测范围保留。 |

任何一行不成立，就不能因为目录数已经达到六个而报告 P12 完成。
