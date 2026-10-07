# Project / Scene Framework：PS0–PS2 R1 补正记录

本轮只处理 PS0–PS2 复审指出的生命周期、身份和组件边界问题；PS3 未开始。
原交付记录保留原样，不能将原 PS1 的 UI ownership 判为已通过。

## 版本

| 项目 | 值 |
|---|---|
| 已审基线 | `5d2ae24f1aaf698b34ee5d42a27cb70db733eec7` |
| 本轮实现与完整资格绑定 | `80979e0966465249c402d4f37a580ed6857b4ec4` |
| lux-cxx | `0a0e7419fc7229df6e372cd35a540249f92250ef`，未修改 |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| 新 SDK | `E:/SyncForder/CodeRepos/install/Framework-project-scene-ps02-r1` |
| 配置 | Windows x64、MSVC 19.44.35228、C++20、RelWithDebInfo、无 RTTI |

全部新资格来自固定提交的独立干净检出，先执行 `ValidateTrackedSnapshot`；没有把工作区中的
用户排版差异带入构建。旧 SDK 和历史输出未覆盖。

## 实际修正与唯一责任

1. LuxEngine 只记录自己挂载的 Project PaneHandle。切换时选择性卸载，把返回的唯一 owner 放入
   transition；Root 继续维护全局 Pane，已卸载的项目 Pane 不再参与路由、维护和绘制。旧 Context
   停止准入并结清 TaskScope 后，先销毁这些 Pane，再销毁 Context，最后挂载候选。旧注册失效时不追踪
   复用槽位或重新挂载的对象。Root 未增加项目分组或所有权标签，原 Process／Runtime 算法未改变。
2. `ProjectDescription::manifest_file` 保存实际读取文件的 canonical 绝对路径；`root` 是其父目录。
   IO 在 canonical 文件上执行，不能在打开任意名称后假定写回 `Project.luxproj`。显式内存 bootstrap
   可以没有文件绑定；它不冒充从磁盘打开的项目。
3. 原 PluginCatalog 的 metadata 身份规则迁至 `project_identity` 的唯一 `PluginIdentity.hpp`。
   Catalog 和 Manifest 都调用 `isMetadataName`，不再复制校验。显示名称仍是人类文本；Scene Profile／
   capability 使用单独的 `isCanonicalSceneName`。没有更改插件身份语法或建立第二份 plugin identity。
4. 原文件读写实现迁至内部 `lux_editor_file_io` STATIC 组件，返回中性文件错误。Project 和 Scene
   各自映射到领域错误，保留系统原因、容量、取消、目标已存在及发布未知。
   `lux_editor_scene` STATIC 提供通用 ScenePackage 文件 IO；它不依赖 Project、Context、Process 或
   具体 Profile。`lux_editor_scene_profiles` 只保留 3D Profile。内部 FileIo 头不安装，实际静态链接闭包
   通过新 SDK 的独立消费者验证。
5. Manifest v1 要求 `format: "lux.editor.project"`；重复 JSON key（含转义后相同的 key）及 root、plugin、
   scene 中未知字段均拒绝。未知版本仍保留准确错误。没有为尚未冻结的旧实验清单新增兼容回落，也没有
   重写用户项目文件。
6. 3D Profile 删除重复的 feature 依赖排序／冲突算法；安装顺序仍由原 FeatureCatalog／RenderSystem
   决定。Profile 保留必需 schema、system、feature 和配置检查。插件说明同步到 `.luxproj` v1。

删除旧 `ProjectFiles.hpp`、具体 profile 目录中的 ScenePackageFile 实现、旧 helper 名字和原插件
validator 算法体；没有同义转发头。28 个实现改动文件可从基线到实现 SHA 的 Git diff 核验。
整个宿主析构仍可清空全部 Root Pane；项目切换和项目关闭不再清空全局 UI。

## 修复前证据

使用原安装 SDK（实现 `1764e1b6ee3b207eb69fbd50dd7833dd66c4e479`，验收 `5d2ae24`）编译并执行真实
消费者，保存原头／库哈希和完整夹具。没有用替身 Context、Root 或声明级探针代替实际运行。

| 命令 | 原实现实际结果 |
|---|---|
| `ps02r1-before-global` | 首次采用 A 后，全局 Pane handle 失效，存活断言失败 |
| `ps02r1-before-maintenance` | 旧 TaskScope 被真实阻塞时，旧 Pane／Element 的维护计数继续增加，断言失败 |
| `ps02r1-before-identity` | `Vendor_Plugin-1` 被实际 PluginCatalog 接受，但被 Manifest 拒绝 |
| `ps02r1-before-codec` | duplicate／unknown 两种输入均被接受，负例返回失败 |

前三项断言退出码为 Windows `3221226505`，codec 负例退出码为 `1`。这些失败不改判为通过。

## 修后行为与工程证据

| 范围 | 实际证明 |
|---|---|
| 全局 UI | 无项目→A→B→关闭项目，全局 handle 有效、持续维护；仅整个宿主析构时销毁 |
| 旧项目 UI | A 的任务收到停止后仍被夹具阻塞；五帧内旧 Pane 和 Element 计数不变，owner 未销毁；完成已交付后才析构，析构时旧 Context 仍有效且已 closed |
| 文件绑定 | 实际打开 `Custom Project.luxproj`，Context 保存精确 canonical 文件及父目录 |
| 插件身份 | 实际 Catalog 与 Manifest 同时接受 `Vendor_Plugin-1`、`Vendor.Render-Plugin`、`_my_plugin`；同时拒绝 `1.plugin` |
| Manifest | strict magic、重复／转义重复、各层未知字段拒绝；原身份、路径、版本、原子写入和中文路径断言保留 |
| 通用 Scene IO | 不链接具体 Profile 的实际 CREATE／REPLACE、读取、损坏包、限制、取消和失败保留；使用原 ScenePackage codec |
| 原行为 | 原有候选失败、取消、发布后取消、Context／服务寿命、Profile 插件代码 lease、2D 扩展、UI/GPU 帧槽及退休回归保留 |

所有全量构建使用 `--target all -j 4 -- -k 0`；构建、测试和实际 GPU 串行。

| 验证 | 本轮结果／原始命令 |
|---|---|
| Editor | 冷构建 1084 步，第二轮无工作；完整 CTest 51/51；`ps02r1-editor-*` |
| PLAYER | 冷构建 1014 步，第二轮无工作；完整 CTest 34/34；`ps02r1-player-*` |
| 新 SDK | 完整构建、第二轮无工作，16/16；含新增全局／维护、Manifest、Profile、跨 DLL 与实际桌面/GPU；`ps02r1-sdk-*` |
| 最小消费者 | 独立 Project、通用 Scene IO、TaskScope 各 1/1；`ps02r1-project-*`、`ps02r1-scene-*`、`ps02r1-service-tasks-*` |
| 公共头 | 38 个安装公共头独立 C++20／无 RTTI 编译；原七类回调约束编译负例保留 |
| 安装产品 | 中文项目创建、重开不改清单字节、WM_CLOSE 正常退出；`ps02r1-product-cli`；未接管鼠标键盘 |
| 实际依赖负例 | 同一 CMake 夹具合法→加入 Scene 到 Profile／Project 的违规边→准确拒绝→去边恢复；`ps02r1-edge-*` |
| 安装与引用闭包 | 新旧路径、legacy、源码私有头补齐和内部 FileIo 头泄漏检查；`ps02r1-closure` |
| 归档 | 555 份文件、60 条已执行命令（含失败）冻结；中文空格路径搬迁通过，删除／篡改真实 SDK 输出均被拒绝；`ps02r1-freeze`、`ps02r1-evidence-verify` |

数量仅辅助列举范围；关键生命周期与边界由上述实际断言证明。此次没有修改 modules 公共头或运行期
算法，不将历史 include 同步、输入实测或工具功能算作本轮成绩。

## 失败保留、范围与工作区

保留修前负例及开发期失败：外部夹具首次配置缺少包内前置组件、摘要格式 API 使用错误；首次源码编译
的 JSON/string_view 比较歧义；新增 Scene IO 取消断言误取 variant 分支。最后一项按实际 encoder 取消
和文件读取取消分别断言，生产错误没有改成统一成功或取消，原断言未删。

原生输入仍为 **NOT_RUN_USER_DEFERRED**；Linux、系统 IME、sanitizer、历史性能延期不变。
未执行 Android 构建或性能长测。注册 Profile 与当前项目可创建性的 UX、nullable bootstrap／借用
Assembly 命名，以及 SceneSession 等 PS3 内容均不纳入本轮。

唯一可变材料：`.internal/editor-redesign/project-scene/`。
外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/project-scene-ps02-r1/`。
归档使用相对路径、文件哈希和固定实现 SHA；缺失／篡改真实必需输出必须拒绝。历史归档不改写。
依赖负例恢复后的全量构建通过，第二轮无工作。固定提交的临时 qualification、修前消费者和预览消费者
构建目录已在证据冻结及哈希核验后清理；主工作区、安装前缀和外部证据保留。

原 EditorContext 成员对齐和 Pane 注释差异保持未提交；资格运行期间额外出现的 LuxEngine／EditorContext
排版调整也单独保存并保留在工作区，没有纳入实现或资格声明。ProjectBuilder 用户补丁仍独立、未应用，
原 SHA256 为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。

实现与本记录分别提交，正常推送实施分支；main 和历史分支不变。停在 **PS0–PS2 R1 复审**，不进入 PS3。
