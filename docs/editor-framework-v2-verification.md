# Editor Framework v2 交付记录

## 范围与版本

V0–V7 的代码迁移及本轮适用 Windows 验证完成，停在 **Editor Framework v2 独立复审**。
不迁入 Scene、Material、Flow 编辑工具，不将历史免验或延期项目改判为通过。

| 项目 | 固定版本／位置 |
|---|---|
| Engine 实现 | `7fcdf1a13cfb4177705e21fef166f1a2487f472f` |
| Engine 基线 | `cf1bd7d64994764490fdafbf1f4215471adb38ee` |
| Engine 分支 | `codex/editor-framework-v2` |
| lux-cxx | `cf14ab1de6b2b96b56531a4de8bfa02efb4d7ca1`，`codex/editor-framework-v2-container` |
| lux-cmake-toolset | `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f` |
| imgui | `7524d14a14ab68b20ac4732db398569482a7096b` |
| 开发目录 | `E:/SyncForder/CodeRepos/lux-engine` |
| 最终安装 SDK | `E:/SyncForder/CodeRepos/install/Framework-v2-final` |
| 实际安装程序 | `Framework-v2-final/bin/lux_editor.exe` |

本记录单独提交。下表中的最终结果绑定上述实现 SHA，而非记录提交或较早的迁移提交。
接口和职责说明见 [框架合同](editor-framework-v2.md)。

## 实际责任与删除闭包

| 责任 | 唯一 owner／存储 | 替代并删除的活动入口 |
|---|---|---|
| 对象身份、消息与回收 | object DLL 的 `ObjectRuntime`；注册表非拥有 | 公开 `ObjectDispatcherRef`、`ObjectIdentity`、产品 queue owner、`ObjectDispatcher.hpp` |
| 父子结构 | `LuxObject` 非拥有父链；删除由实际 C++ owner 执行 | `EObjectOwnership`、`OwnedEdge`、`adoptChild/adoptChildren`、父托管销毁及失去消费者的 deleter |
| 可停靠窗口 | `Root` 的 `SlotKeyAutoSparseSet<PaneId, unique_ptr<Pane>>` | `EditorUIRoot`、`PaneHandle`、`PaneTypeId`、attachment epoch、旧 `Attachment.hpp` 提交协议 |
| 窗口内容 | Pane 单内容根；Element/Layout 的成员或唯一 owner | 内嵌子 Pane、强制 parent/dispatcher 构造、全局 Element registrations/holes/compact |
| ImGui 后端 | UI 私有 `Context` | Root 中对应后端算法原体；无第二份输入读取或绘制实现 |
| 项目与工厂 | `EditorContext` 的三个独立 Registrar | `UiTypeId/UiDescription`、工厂发放 PaneId、第二套顶层 UI owner |
| 错误描述 | error DLL 进程共享的不可变 Registry | `FrameworkFailure/EFrameworkError` 及框架、Scene 跨系统 `std::any` cause |

`ObjectState`、连接控制块、`CodeLease`、真实共享 allocation 回收、领域本地错误、
`RenderErrorRegistry`、Process、SceneRuntime 及 GPU 退休机制仍保留各自职责。
Render 错误在描述可解析时转换，临时槽位不充当稳定 ErrorId。

基础 target 增加 `modules/core/error` 的共享描述提供者；既有 Editor 三个 STATIC 框架 target
继续存在。没有新增旧产品兼容 alias、旧 ABI 回落或新的 Editor 服务容器。

`editor_legacy` 保留冻结参考源码及历史消费者；当前构建入口拒绝开启 legacy。
外部渲染插件的旧 Editor target／descriptor 生成分支已删除。25 组旧 SDK 消费者移入冻结目录，
当前消费者保留 Object、UI、services、spatial 和新框架资格。
文件迁移清单采用 Git rename-aware 输出；不能把迁移的删除／新增计数当成被消灭的算法数量。

外部交付目录中的 `file-migration-7fcdf1a1.txt` 和 `implementation-inventory-7fcdf1a1.json`
记录逐文件增删改。新安装及依赖前缀检查没有发现 legacy provider、废弃公共头或旧 Engine SDK 回退。

## 最终执行结果

独立干净检出先通过 `ValidateTrackedSnapshot`，使用 MSVC 19.44.35228、C++20、
RelWithDebInfo、Ninja。构建统一为 `all -j 4 -- -k 0`；构建与 GPU 执行串行。
隔离依赖 SDK 重新安装，未借用另一份 Engine 安装补齐缺失头或库。

| 验证 | 本轮实际结果 | 外部命令记录前缀 |
|---|---|---|
| clean Editor 全量构建 | 首轮 1046 个动作成功；第二轮无新增工作 | `v7-final-first-all`、`v7-final-no-work` |
| Editor 完整适用 CTest | 39/39，通过真实 GPU 和自动桌面测试 | `v7-final-ctest` |
| PLAYER | 全量 1006 个动作成功；第二轮无工作；33/33 测试通过 | `v7-final-player-*` |
| 全新 SDK 框架消费者 | 7/7；Object、Error、services、Context、UI、GPU、desktop | `v7-final-sdk-*` |
| 活动独立 SDK 消费者 | 六组共 9 项通过；含真实 DLL、任务、生成器 | `v7-consumer-*` |
| 外部 render 插件 | 安装 SDK 构建并加载真实 DLL，GPU 测试 1/1 | `v7-plugin-*` |
| 修改后的安装公共头 | 38/38 独立 C++20、无 RTTI 编译通过 | `v7-final-public-headers` |
| 实际编译／链接闭包 | Editor 542、SDK 33、插件 3、spatial 5、PLAYER 519 个编译单元检查通过 | `v7-final-closure` |
| legacy 负例 | 开启旧选项在指定冻结边界明确拒绝 | `v7-legacy-negative` |
| 公共头同步 | Debug、RelWithDebInfo、Android 及活动前缀按清单同步／清除废弃头 | `v7-public-header-sync` |

六组独立消费者是 object-core、object-ownership、ui-composition、services-core、services-tasks、
spatial。安装目录仅提供一个 `lux_editor.exe`。Android 仅同步公共头，不具有本轮构建资格。

### 行为对应关系

测试数量不替代合同；以下是实际重复运行的主要验证入口。

| 合同 | 真实测试与保留的危险条件 |
|---|---|
| 容器身份 | lux-cxx 原测试：代际 clear、回收槽位耗尽、fresh index 耗尽、容量准备和唯一 owner 提取；保留修前四项失败 |
| ObjectRuntime | `object.queue/tree/ownership/runtime_shutdown/retirement_contract`：线程拒绝、固定批次、FULL、接收撤销、重入、跨线程末引用和真实 DLL 清理尾部 |
| Root 所有权 | `ui.root/composition/structure_contract`：批量拒绝不消费、代际失效、移除再挂载、单内容根、拓扑拒绝、结构冻结与实际析构次序 |
| UI 后端 | `ui.context`：输入批次／sequence、Context 激活恢复、输出独立寿命；无第二全局控件表 |
| 项目切换 | `framework.context/ui` 和相应 SDK 测试：惰性服务、Registrar 失败、VFS 隔离、UI 先于 Context 释放及失败后的无项目状态 |
| Error | `error.registry`、`render.error_conversion`、`sdk.error`：幂等／冲突、并发、地址稳定、未知错误、DLL 卸载后解释、真实注册碰撞分支 |
| Scene 失败 | `scene.driver/runtime/world_loading`：类型化错误转稳定 ID/参数后保留原失败、取消、容量和实际执行事实 |
| 无 RTTI | `build.no_rtti`、安装头与消费者：合法编译，然后拒绝 dynamic_cast/typeid；Flow 能力及 RenderFeature 窄接口回归 |
| 图形与退休 | `framework.gpu/desktop`、`render.runtime_resources`、`scene.render_resources`、真实外部插件：UI 像素回读、已捕获帧、资源责任及退休 |

原多对象线程域和父托管销毁测试按 v2 合同替换。队列、代际、回调、代码寿命、
失败不消费和 GPU 退休断言继续保留。碰撞测试进入实际 Registry 碰撞分支，
不声称发现了自然发生的 FNV 碰撞。

自动 desktop 测试覆盖 resize、最小化／恢复和关闭；不等同于鼠标键盘接管或系统 IME 验收。
高亮相关改动运行了真实后端绑定回归，不将其扩大为新增高亮像素隔离场景。

## 有限成本与失败证据

ObjectId／PaneId 使用同一已修正的稀疏容器；预备后的唯一 owner 插入／提取不重新发号或分配。
Root 从窗口开始一次遍历内容树，稳定维护不创建全树快照或重建全局登记；
运行期窗口标识预留后使用 `to_chars` 生成。代码和合同测试支持这些局部结论，
不宣称整个应用、ImGui 或所有业务路径零分配，也没有重启历史性能长测。

迁移中的编译失败、旧断言冲突、早期隔离前缀的绝对路径问题、首次独立配置缺少 Lua 路径、
原始独立头探针遗漏依赖编译参数均保留在原命令记录中。
完成修正后的最终 clean 全量构建与矩阵绑定 `7fcdf1a1`；此前 `35de4fc89` 的结果不替代它。

## 外部证据与用户差异

交付目录：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2`。
命令、原始结果、哈希、构建元数据和清理收据均在该目录，仓库不再复制完整日志。

| 不可变历史归档 | SHA256 |
|---|---|
| `historical-evidence-cf1bd7d64994.zip` | `0852b321d37180aa43863f3147a53cd1cedf89cf8b41b3d641311b8c89bf2cff` |
| `historical-working-materials.zip` | `107ec53cc979690a15787427e20ec06ded6da20b4b2b81d255ab619bb765ab11` |

前者保存 15,754 个原文件，后者保存 35,931 个施工文件；全量条目已在迁移时核验，
结束时再次检查归档哈希。`qualification-7fcdf1a1/manifest.json` 固定本轮 464 份构建、
链接、安装及实际测试输出。证据在中文／空格路径验证，并实际拒绝缺失和篡改的输出。
临时 qualification 检出及构建树在冻结并核验后清理；常规开发目录和最终安装 SDK 保留。

`ProjectBuilder.cpp` 用户文件 SHA256：
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
补丁、原字节、原 blob 和映射文件单独保存在 `protection/ProjectBuilder/`；
对应 `editor_legacy/authoring/project/src/ProjectBuilder.cpp`，**未应用**。
`Pane.hpp` 原有注释缩进差异仍在开发工作树，未计入实现或本记录提交。

原生输入接管仍为 `NOT_RUN_USER_DEFERRED`。Linux、系统 IME、sanitizer 及历史性能未完成项
维持原判定。历史 PARTIAL、FAIL、免验和阶段结论不改写；本交付只报告上述实际执行范围。
