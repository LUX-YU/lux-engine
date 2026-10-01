# P10Q 实施与验收报告

本报告由本阶段唯一施工账本和原始命令记录汇总。最终实现为 `3c20910d05d635078ef9021a3a3318086a792b8d`。

**Windows 功能与工程验证通过；P10Q 整体 PARTIAL。** Linux 必测未执行；用户明确结束旧版 50k 深链长测，保留 10 次预热及 66/100 次计时样本，因此性能项和 XQ24 也为 PARTIAL。系统 IME 为 NOT_RUN。不得据此进入 P11。

## 最终验收结果

| 项目 | 实际结果／证据 |
|---|---|
| 干净 tracked commit 全量构建 | 首轮 1377 步成功；第二轮无工作；显式 `LUX_EDITOR_MIGRATION_STAGE=P10Q`。logs/build.log、no-work.log、configuration/。 |
| 完整 CTest | 204/204；包含原 195 项名称，行为／断言迁移见 behavior-map.md，不能只依据数量。 |
| PLAYER | 全量 942 步、二次无工作、11/11；编译数据库无 Editor 依赖。 |
| CPU native | 全量 1339 步、二次无工作、178/178；GPU／桌面／工具链运行测试关闭，生产功能仍构建。 |
| 新 SDK | 原 14 组全部通过；另有四个质量消费者与两个旧显式 GPU 模式。八项 operation 编译负例和实际依赖负例保留。 |
| 第二编译器 | clang-cl 19.1.5 的四个质量消费者通过；38 个改动后的安装公共头逐一 C++20 独立解析通过。解析与真实链接资格分别记录。 |
| 新双视口／原生输入 | 安装消费者真实运行 757 帧，验证层错误 0；OS 鼠标／键盘、capture、焦点、Inspector 编辑及 Undo 通过。系统 IME 未测，用户手测正常单独记录。 |
| 真实文件 IO | 原发布／Workspace 回归通过；四个并发场景各用独立目录成功，无自动重试。原 Access denied 证据保留，不宣称其 OS 根因已证实。 |
| 性能 | BQ1–BQ5、分配／共享字节／真实画布／容器资格完成；仅旧 50k 深链耗时样本按用户指示停在 66/100，整项 PARTIAL。 |
| SDK 同步 | 新前缀独立消费后，同步开发前缀并逐文件清除废弃接口；没有 modules 公共头变化，不触发三个 include 前缀同步。 |
| Linux／ASan | Linux Editor／PLAYER／IO／Host／工具链／SDK 为 NOT_RUN。补充 consumer-only ASan 因与未插桩 SDK 的 STL annotate_string 链接不一致而未形成资格，未禁用该检查。 |
| 历史失败 | C01、C03、C04 仍退出 1、保持原 FAIL 与后续责任。 |

## 范围与基线

只执行 P10Q，停在 P10Q 等待复审。输入验收提交为 `b583e7ffe20e7a1ac55c7119d6a13ac337ebb323`；未进入 P11，未修改 main。历史 P00–P10/R1 快照原样保留。

用户的 `editor/project/src/ProjectBuilder.cpp` 修改没有纳入实现提交，其 raw SHA256 仍须为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。最终资格使用独立 clean clone，因而不借用该修改。

lux-cxx 固定为 `aae62e2fc17ef78ae7be2559a6d58fa0f8297b05`，没有降级到输入包引用的旧版本。imgui、node-editor、toolset 的实际提交及安装输入记录在 baseline 和 dependency-seed 中。

## 唯一责任与删除结果

| 事实／资源 | 唯一 owner | 消费者及被删除的重复责任 |
|---|---|---|
| 项目目录数组、AssetId 索引和 revision | ProjectStorage 组合的 ProjectCatalogModel | ProjectView、AssetPicker、Inspector、ResourcePane 借用同一 Model；删除 ProjectCatalogAccess、ProjectCatalogAdapter 和 Storage 的重复目录信号。assetContentChanged 仍是 Storage 的文件内容事实。 |
| 目录某版本的寿命 | ProjectCatalogSnapshot 的不可变共享 Data | 同版本多消费者共享数组；候选排序和索引完整成功后才采用。错误保留上次目录，通知失败通过版本复查恢复。 |
| 后台任务事实 | 原 ExecutionRuntime | TaskMonitor 只拥有一次 observer 安装／撤销、通知合并和版本查询缓存。TaskView、旧 TaskPane 与 Context 共用 Monitor；删除 TaskQueryPort、裸 revision 回调和无条件全量轮询。 |
| Material 编译请求与确认 | MaterialCompilationService 中有界 unique_ptr operation 记录 | MaterialView 借用服务；harness 不再拥有同一业务算法。Flow 使用已有 FlowCompilationService，未新增对称控制器。 |
| 预览采用、资源与退休 | 原 MaterialPreviewStore、RenderResources、SceneRuntime | 不并入编译服务；视图关闭不取消应用仍拥有的后台任务或 GPU 责任。 |
| 已编码产物准入 | 同一个 WriteCoordinator／publishEncodedArtifact | 两种领域自由函数只形成准确产物和目标；删除无状态 Operation 类和重复 reserve/provide/失败清理算法。 |
| 文件读写与真实发布 | FileArtifactStore／原 FilePublication | ProjectArtifactStore 改名迁回 storage；SaveExecution 迁入 persistence。删除 adapters/project_io 的目录、target、包和 namespace 接口。 |
| 视口、相机及高亮后端 | editor/views/viewport | ViewportElement 替代公共 SceneElement；SceneView 与 MaterialView 共享实现。sceneCreationPoint 留在 Scene 工具，Material 不依赖 Scene UI。 |
| 关闭准备与失败事实 | DetachedView 的准确结果、ViewHost 的关闭记录 | BUSY 保留并续行；永久失败保留诊断但停止自动重试。Host 不推算领域错误，不另建资源退休责任。 |
| 输入阶段与载荷 | 小型内部 deliverInput 算法；各工具仍拥有自己的队列 | based_on 与草稿捕获戳不可变；共享阶段交付不将作者戳引入 widgets，不改变领域 gate。 |
| 历史、会话、运行与保存基线 | 原 History、SessionStore、SessionState、RunStore 与 WriteCoordinator | 没有第二套 current、dirty、busy、History、Runtime、线程池或完成交付路径。 |

完整新增／修改／迁移／删除清单由 input_sha 到 implementation_sha 的 Git 差异生成，见 files.json 和 file-moves.tsv；迁移后的生产及 SDK 消费者逐调用位置见 logs/extra/migrated-consumers.log，具体编译依赖见 File API 清单。旧文件迁移不留转发头或命名空间别名。三个 public SessionAccess 纯别名头被删除，真实 detail::*SessionAccess 实现保留。

## 检查与借用的有效期

本轮不以减少分支为目的删除 gate、版本、文件冲突或异步采用检查。

| 位置 | 原检查／重复工作 | 首次事实与有效期 | 可能失效的操作 | 最终处理及证据 |
|---|---|---|---|---|
| ProjectView／AssetPicker | 函数表 owner／回调是否为空；每视图复制目录 | 构造必须借用 Model；快照共享 owner 固定版本 | 项目替换、revision 变化、输入来自其他项目 | 删除不完整函数表状态；外部 AssetReference 的项目、版本、存在和类型校验保留。目录测试覆盖替换、BUSY、FULL/CLOSED 和 resync。 |
| TaskListElement | nullable revision fallback 每帧 taskInfos | 构造借用唯一 TaskMonitor；缓存只对对应 revision 有效 | task observer 的变化／resync | 删除全量轮询分支；Runtime 仍判定取消准入和终态。双视图及视图关闭后完成测试保留。 |
| Material／Flow UI | 可空 compile/publish function pointer 与通用 action 分支 | 服务引用在构造时成立；操作与目标分别明确 | Session 代际变化、来源变化、编译完成、发布冲突 | 删除空服务分支和 void* 转发；未绑定内容与空发布目标仍显式不可用。准确错误和 ticket 不混用。 |
| InspectorFields::withRead | 即时 owning callback 的构造／分配 | function_ref 仅借用当前栈上的具名回调 | 回调返回即失效；跨帧保存禁止 | 即时借用，延迟回调仍 owning。字段写入、捕获及扩展回调的原 gate 保留。 |
| deliverInput | 两份相同阶段分支 | 当前调用验证入队时 based_on；阶段成功后才前移 | 回调、BUSY、内容改变、取消、下一次维护 | 合并算法，不把一个阶段的验证缓存为跨回调事实。原 R10-R1 十个真实 UI 场景和交互回收回归验证。 |
| FlowInteraction::synchronize | 内容未变仍冻结全图，仅为查找已验证的选择 ID | select 或同步成功时固定 selection_source_；仅对同会话、同内容有效 | 编辑、Undo/Redo、重载、关闭、Store 准入改变 | 替换原 history-only 选择来源；每次仍借用 Store 并进入原 gate，只有内容相同才复用 ID 存在事实。变化后仍冻结校验，失败不更新来源。没有并行 current／dirty。 |
| publishEncodedArtifact | 两份同义事务接线 | reserve 的 ticket 只标识已接纳记录 | 文件变化、Unknown、取消、容量和采用期回调 | 合并接线而不移除 WriteCoordinator 的对外检查。P05/R1/R2 的真实 IO 和递归交付回归保留。 |

没有新增可长期保存的 Validated/UnsafeAccess，也没有用 assert 代替真实失败出口。对尚无测量收益的 Store 查找和线性小记录不做推测性容器改写。

## 链接与安装边界

实际 File API 和链接命令表明：`editor_project`、`editor_app`、`editor_scene`、`editor_material`、`editor_flowforge` 五个产品内部边界由 SHARED 改成显式 STATIC。不能把新增目录或逻辑 target 数量当成 DLL 数量。

History／Session 的共享身份状态、Metadata／反射登记、真实插件与动态消费者需要的边界保留 SHARED。旧 editor_ui 和窄 editing 仍有过渡消费者，没有为了数量强行合并到 exe；P12 删除期限不变。新增 viewport 为 STATIC，persistence 主题保留纯协调和执行接线两个 STATIC target，使纯消费者不必引入 Process。

所有改动后的公开接口在全新前缀安装和消费；安装闭包不借用旧 build DLL 或旧 Engine 头。第一方 C++20、禁扩展、MSVC 严格模式、PIC、导出宏和静态依赖传递统一。没有 whole-archive、全符号导出或 C++23 新依赖。

首次 tracked cold build 发现 scene_execution_api 的 RenderSceneId 头由旧安装树掩盖了缺失依赖。正式 target 及安装 find 已补 render_client；原白名单允许 core 头却漏记 provider 的不一致一起修正。新增实际负例仍拒绝 render_runtime 直接及传递进入窄执行／交互接口，没有放开整个渲染模块。

## 性能与复杂度

四项确定整改已实施：全图层级检查改为迭代三色 O(N+E)；目录／任务共享 revision 数据；冻结产物采用 SharedBytes；图画布按安全点重建后端和身份映射。局部 reparent 仍 O(height)，没有把每次字段编辑改成全图构建。

实测后还收敛了 Flow 选择稳定同步的整图冻结：选择来源从 HistoryId 收窄为完整内容戳，复用的仅是已验证的 ID 存在事实。Store 与会话 gate 每次仍检查，作者模型和 frozen-source 协议未改；发生内容变化时继续走原快照校验。删除／Undo／Redo／关闭及嵌套 READING 的行为回归必须通过后才采用该优化。

实际 parent 读取计数在 50k 深链上由 1,250,025,000 降到 50,000；新验证 p50 为 5516.3 µs。计数使用固定源码的测试插桩，耗时使用未插桩 SDK，二者不混用。1000 节点的 Flow 稳定同步可计数分配由 9034 次降到 1 次／32 字节，不宣称为零。1／16／64 MiB 冻结载荷均为 110/110 次沿用原字节 owner、零 payload 再复制。真实画布完成 10k 替换及四次安全整理；隔离映射末尾保留 1317 项，旧映射为 30001 项。完整原始数据及范围见 performance.md。

BQ1–BQ5 的实际数据、热身、轮次、输入规模、机器和覆盖范围由 performance 汇总。全图算法为线性额外索引付出 O(N) 空间，不能宣称它减少分配。目录旧稳定帧已经没有全量复制，新增收益是多消费者的初始及版本变化共享；旧任务无 revision 路径才存在每帧全量复制。

C++ new 计数只覆盖标明的测量线程、可执行文件及静态 archive，不包含另一个 DLL 的 CRT、malloc 或 GPU 内存。buffer 地址不变只证明没有替换该缓冲，不能单独证明整个进程零分配。墙钟分位数附带同时存在的构建／长基线进程负载范围，不宣称无噪声固定提速百分比。

Canvas 测试除 10k churn 外，还保留大 ID、在途输入、实际鼠标选择、作者布局、pan／zoom，以及重建后的首帧和后续帧。扩展验证发现的选择通知时机和 node-editor 首次测量光标问题，通过公开设置接口与 End 后读取修正，没有修改第三方依赖或使用其私有实现头。

## 平台、历史问题与未完成资格

Windows 的正式状态以最终命令记录为准。Linux 是必测但本轮按用户确定的顺序没有安装或运行环境，因此 P10Q 整体只能 PARTIAL。clang-cl 是 Windows 第二编译器检查，不能代替 Linux。系统 IME 未实测仍 NOT_RUN；用户报告手工测试正常与自动原生输入结果分别列出。

C01、C03、C04 保持原 FAIL 和原后续责任；新发现的问题不挂到旧编号。P10-R1 Workspace Access denied 首次证据保留：本轮修正了多进程夹具共用目录的确定问题，但没有证明当时的 OS 根因，不增加掩盖失败的自动重试。

当前阶段仍保留 P12 到期的旧 Context／Editor／工具 UI 转换壳及登记消费者。新整合链不依赖这些私有桥。没有提前实现动态扩展、产品入口切换或 P10/P13 之外的资格替代。

账本中 `EditorContext::project_tasks` 的原“通知”归类已纠正：这是 ProjectStorage 读取所借用的 TaskScope，保留到原项目生命周期迁移，不迁入 TaskMonitor，也不误删。已删除的 ProjectCatalogAdapter 从活动桥路径中移除，并记录其 P10Q 删除 SHA；原 P10 快照不变。

## 交付

实现按提供者、迁移、链接、C++20／回调、性能、门禁拆分提交；冷构建发现的具体遗漏使用追加修正提交，不改写历史。验收绑定最终一个 implementation_sha，之后单独提交 dev_log/P10Q 并推送实施分支。归档验证器按相对证据路径和固定 Git 对象校验，缺失／篡改必须失败；所有历史快照保持原样。

最终归档实际验证：完整材料复制到含中文和空格的新路径后通过；删除真实 CTest 日志、篡改该日志分别被拒绝；恢复原字节后通过。命令和输出见 archive-probes/，正式原路径复核通过，状态仍为 PARTIAL。此项证明证据可迁移，不改变 Linux、性能或 IME 状态。
