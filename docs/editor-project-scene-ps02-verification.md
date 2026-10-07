# Project / Scene Framework：PS0–PS2 交付记录

按 `lux-engine-project-scene-framework-implementation.md` §127 完成第一交付入口。
本轮完成 Project 持久化与异步切换、拥有型产品装配、SceneProfile 注册和真实 3D 作者场景包。
PS3–PS9 未开始；没有创建项目 SceneRuntime 实例、SceneSession、EditorScene 或具体编辑工具。

## 版本与范围

| 项目 | 版本／位置 |
|---|---|
| 基线 | `c0637ad68b841ccd8ad3cc46af02a4122d003753` |
| PS0 | `b36aa3272c28b221722872894b070ab1b9ab963e` |
| PS1 | `b48db4156d38159afc8a623fa6d78d349b1fb573` |
| 完整实现、最终构建与 SDK | `1764e1b6ee3b207eb69fbd50dd7833dd66c4e479` |
| lux-cxx | `0a0e7419fc7229df6e372cd35a540249f92250ef`，未修改 |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| 全新安装前缀 | `E:/SyncForder/CodeRepos/install/Framework-project-scene-ps02` |
| 依赖前缀 | `E:/SyncForder/CodeRepos/install/Framework-v2-dependencies`，不含 Engine SDK |
| 编译配置 | Windows x64、MSVC 19.44.35228、C++20、RelWithDebInfo、无 RTTI |

原规范归档 SHA256：`2074456c12b28590a121c63e16d6f16eed800721e8887b63db49ab16130d85a2`。
旧资格按各自 SHA 保留；下表成绩均来自本轮实际执行，不引用旧数量作为新成绩。

## 实际实现与唯一责任

- `lux_editor_project` 提供版本化 ProjectManifest、纯验证、JSON codec、有界读取和原子单文件
  CREATE／REPLACE。保留项目 UUID、Scene AssetId、插件版本、相对物理路径、profile 和 startup scene。
  拒绝路径穿越、Windows 路径别名、重复身份、未知版本和非法输入。未知发布结果仍是明确错误；
  没有恢复旧保存框架或跨文件事务。
- LuxEngine 持有 `EditorAssembly`，每次候选 Context 使用同一拥有型装配。私有 transition 通过
  原 Process blocking scheduler 读取／创建清单、验证插件并准备不可变注册值。完成只移动拥有型结果；
  主线程安全点才构造 Context 和离树 UI。读取、插件或候选工厂失败时，原项目继续可用。
- EditorContext 拥有清单、独立 VFS、PluginManager、SceneRegistrations、独立 Registrar、服务和项目
  TaskScope。关闭先停止准入，跨帧等待已接纳工作结清，再销毁 UI 和 Context；服务及插件代码保留到
  完成回调返回。原 TaskScope 仅增加 owner-thread `settled()` 查询，不等待、不派发、不改变准入。
- SceneProfileRegistrar 保存冻结的描述与可选代码 lease；复制描述保持代码寿命。具体
  `lux_editor_scene_profiles` 提供 3D preset，通过实际项目注册的 schema、system、feature 和 codec
  创建 ScenePackage。它包含单分区 World、五种作者 schema、Transform／WorldLoading／Render 三系统、
  空 Simulation 和五个渲染 feature 配置，不创建编辑器相机或运行实体。
- ScenePackageFile 复用原 ScenePackage/Pak/world codec 和同一个文件发布实现；未知 Pak 条目保留。
  稳定宿主不链接具体 3D Profile，产品 main 显式选择它。测试 2D profile 走相同公共入口。

替换旧的内存项目／每次 open 传装配入口及全部活动调用；不留旧入口兼容壳。新增的是纯项目组件和
具体 profile 组件，没有新增线程池、Runtime、通用 Manager、第二任务库或 Scene 文件格式。
原 Object/UI/Error、History、渲染帧槽、SceneRuntime 驱动及退休实现未重做。
全部 42 个实现改动文件列在外部 `files.tsv`，对应 Git 区间为基线到完整实现提交。

## 行为证据

| 范围 | 真实入口与断言 |
|---|---|
| PS0 | 空／单／多 Scene 清单往返；重复 ID／路径、非法 profile／路径、未知版本、容量；实际 CREATE／REPLACE、取消保留原文件、失败清理临时文件；中文文件路径 |
| TaskScope | 已结束但未收取仍不 settled；查询不调用完成；收取后 settled；查询不关闭后续准入；原服务任务和 PLAYER headless 场景继续通过 |
| PS1 | 创建 A；缺失文件／插件及取消保留 A；B 准备后，A 的阻塞任务未结束时连续 frame 返回且服务存活；完成后 UI 先于服务析构；引擎／窗口／Root 地址不变 |
| 候选失败 | C 的第二个 UI 工厂拒绝；已接纳任务的完成仍到达，C 的服务在回调内存活；C 候选清理后 B 完整保留 |
| 磁盘与采用 | worker 已发布清单但尚未收取时取消，状态记录 CANCELLED 和 confirmed publication；磁盘文件仍存在，旧项目不变 |
| 窗口关闭 | 打开请求在途时关闭原生窗口，继续结清，迟到项目不采用；原 UI Scene 保持唯一实例，resize／最小化／帧背压继续成立 |
| PS2 | 冻结、重复／无效／未知 Profile；复制保活；真实插件管理器释放后注册值仍能调用 codec；worker 创建 3D 包；缺失 schema／feature／system／binding 拒绝；停止拒绝 |
| Scene 文件 | 实际 encode/write/read/decode；完整作者描述校验；未知成员字节保留；容量／取消失败不覆盖旧包；2D 替换仍走同一格式 |
| OCP | 测试 2D profile 不修改宿主；3D 包创建前后运行实例数均为零、无 RenderContext；产品 UI 泛化枚举 Profile |

## 最终工程矩阵

从独立、干净、固定提交检出执行 `ValidateTrackedSnapshot`。所有全量构建使用
`--target all -j 4 -- -k 0`，构建和实际桌面/GPU 顺序执行。

| 验证 | 实际结果／记录 |
|---|---|
| Editor 冷构建 | 1076 个构建步骤；第二轮 `no work to do`；`ps02-editor-all/no-work` |
| Editor CTest | 47/47；包含真实插件、文件 IO、UI Scene GPU、桌面切换及原框架回归；`ps02-editor-tests` |
| PLAYER 冷构建 | 1014 个步骤；第二轮无工作；编译清单没有 Editor 源；`ps02-player-all/no-work` |
| PLAYER CTest | 首轮 33/34，契约子进程超时见下文；原二进制定向复核通过，完整重跑 34/34；`ps02-player-ui-contract-02`、`ps02-player-tests-02` |
| 新 SDK | 全量构建与二次无工作，12/12；包含 Context 单组件、真实插件 Profile、跨 DLL Object/Error、UI 与桌面；`ps02-sdk-*` |
| 独立消费者 | 只链接 Project 的清单消费者 1/1；Process 服务任务消费者 1/1；`ps02-project-*`、`ps02-service-tasks-*` |
| 公共头 | 37 个 SDK 公共头独立 C++20／无 RTTI 编译，含新增 Profile 和 TaskScope；源码与 SDK 保留原六类回调编译负例，并增加 Profile 可抛工厂拒绝 |
| 安装产品 | 唯一 `lux_editor` 通过中文目录／名称创建、重新打开、原生关闭；`ps02-product-cli-02` |
| 闭包 | Editor 563、PLAYER 526、SDK 50、两独立消费者各 1 个编译单元；无 legacy、旧 SDK、源码私有头补齐；纯 Project 不依赖 Process/Runtime/UI，宿主不依赖具体 Profile；`ps02-closure` |
| 归档 | 526 份输出及元数据、60 条命令冻结；中文空格路径搬迁通过，删除／篡改真实 SDK 输出均被拒绝；`ps02-freeze`、`ps02-evidence-verify` |

安装产品检查只通过系统关闭消息结束自己的测试窗口，没有接管鼠标键盘。它证明 CLI 创建／重开及
关闭路径，完整项目采用与生命周期由 SDK desktop 测试直接断言，不以进程退出码替代。

## 保留的失败和限制

- 开发期的 UUID 随机源类型、显式 include、测试字段与返回类型错误，以及未知条目夹具使用非法 VFS
  路径的失败均保留。修正夹具使用原合法 VirtualPath，没有修改冻结的 VFS／Pak 规则或减少断言。
  一次错误正则导致未选中测试的调用也保留，不计为通过。
- 首次附加产品检查在重新打开时，采用了进程启动初期的任意窗口列表；实际存在 GLFW message window
  等辅助窗口。限定为标题为 LuxEngine 的产品窗口后通过，原超时和旧夹具没有覆盖。
- PLAYER 首轮 `destroy-in-event` 子进程在 15 秒超时。Windows 1000/1001 记录显示，13:56:43 已触发
  预期 `0xc0000409` fail-fast，13:56:58 完成错误报告，覆盖了测试的等待窗口。保留系统记录及首轮
  失败；没有修改 UI、负例超时、终止断言或安全软件。同一二进制定向复核和完整重跑随后通过。
  此记录不宣称消除了系统错误报告的延迟波动。

原生键鼠实测继续 **NOT_RUN_USER_DEFERRED**；Linux、系统 IME、sanitizer 和历史性能延期保持原判定。
没有 Android 构建，也没有新性能长测。本轮未修改 modules 公共头；唯一底层 API 增补在 engine 的
TaskScope，已由全新 SDK 安装和独立头／消费者验证。既有模块头的同步记录不冒充本轮新资格。

## 保护与交付

唯一可变施工材料仍在 `.internal/editor-redesign/project-scene/`。外部证据位于：

`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/project-scene-ps02/`

其中 `verified-evidence/manifest.json` 使用相对归档路径和字节哈希，不依赖生产机器上的原日志路径。
原始命令与退出码同时保留在上一级 `runs/commands.json` 及 `runs/logs/ps*.log`。
临时 qualification 检出在冻结并核验后清理；SDK、证据和主开发工作区保留。

EditorContext.hpp 的原成员对齐和 Pane.hpp 的原注释差异仍在工作区，均未纳入实现提交。
ProjectBuilder 用户补丁保持独立、未应用，原用户字节 SHA256 为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
main、历史分支和既有验收记录未修改。

**PS0–PS2 实现与上述 Windows 资格完成，停在第一交付复审。** 后续 SceneSession、多场景加载／运行、
Tool 组合和保存／历史业务按原分期继续，不能把本轮包创建测试解释为这些功能已完成。
