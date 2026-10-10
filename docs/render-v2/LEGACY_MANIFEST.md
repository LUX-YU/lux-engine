# Render V2 R0：冻结来源与迁移清单

本文件是冻结事实与恢复入口，不是可变施工状态账本。实施资格见独立的 `R0_VERIFICATION.md`。
本次只实施 R0；R1–R18 未授权。日期：2026-10-10。

## 来源、隔离与状态

- V1 frozen SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`。
- 来源分支：`codex/editor-framework-v2`；开工时远端与本地 HEAD 均等于上述 SHA。
- V2 分支：`codex/render-v2`，从上述 SHA 新建；没有修改历史标签、main 或远端分支。
- V2 施工 worktree：`C:/Users/ChenHui/.codex/worktrees/render-v2/lux-engine`。
- V1 oracle：`C:/Users/ChenHui/.codex/worktrees/render-v1-oracle/lux-engine`，detached、固定上述 SHA。
- V1 状态：`REFERENCE_WITH_KNOWN_LIMITATIONS`；冻结不表示旧 MA 最终资格通过。
- V2 产品状态：`EXPECTED_UNAVAILABLE_UNTIL_R17`。Root 仍引用已迁出的旧目录，R0 不修补这些接线。
- R0 没有新的 Render V2 生产 C++；没有可运行的 V2 渲染器或可安装的 V2 SDK。

原工作区 `E:/SyncForder/CodeRepos/lux-engine` 保持原分支与六处用户差异，不参与施工或资格构建。
保护快照、binary diff、未跟踪清单和逐文件 SHA-256 位于源码树外：
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R0/preflight-20261010/`。
两个 Render 用户修改不进入冻结源码；未执行 stash、reset、覆盖或 cherry-pick。

## 精确迁移闭包

全部使用 `git mv <source> render_legacy/<source>`，保持相对层级与原 Git blob。

| source | destination | 文件 | Git blob 字节 |
|---|---|---:|---:|
| modules/function/render | render_legacy/modules/function/render | 663 | 38,049,715 |
| engine/context/rendering | render_legacy/engine/context/rendering | 5 | 10,168 |
| engine/scene/builtin_systems/render | render_legacy/engine/scene/builtin_systems/render | 29 | 280,128 |
| modules/function/ui/rendering | render_legacy/modules/function/ui/rendering | 10 | 44,703 |
| engine/project/plugins/rendering | render_legacy/engine/project/plugins/rendering | 4 | 15,836 |
| examples/render-plugin | render_legacy/examples/render-plugin | 8 | 38,730 |
| 合计 | | 719 | 38,439,280 |

机器清单：[LEGACY_SOURCE_MANIFEST.json](LEGACY_SOURCE_MANIFEST.json)。
SHA-256：`b4a4cb0b92479ba1b91e99c76aa0f26a1fd01820577a2b0855d2b435613fe91c`。
清单来自 `git ls-tree -rlz <frozen-SHA> -- <six-roots>`；按原路径排序，记录 source、destination、
mode、Git SHA-1 blob identity 与大小。总量包含 shader、资产、生成器模板、测试与 CMake，不只是 C++。
清单自身 SHA-256 使用原始 UTF-8 JSON 字节计算；源码一致性以 Git blob 为准，不将 Windows checkout
的 CRLF 转换误判为算法修改。archive 唯一新增文件是顶层防误用 `CMakeLists.txt`。

## 输入规范与命名映射

[INPUT_MANIFEST.json](INPUT_MANIFEST.json) 记录 ZIP SHA-256 和 12 个原名/目标名/内容哈希。
00–11 按 R0 工作单的英文名称导入，正文逐字节保留；独立 R0 工作单与 ZIP 内对应文件一致。
未另存旧 R1 或中文文件名的第二套规范。原文中的中文相对链接可能因映射失效；查阅时使用下表，
不借修链接改动规范正文。附件内历史授权叙述不取代本次用户明确批准的 R0 范围。

| 原文件编号与主题 | 仓库文件 |
|---|---|
| 00 总览 | [00_README.md](00_README.md) |
| 01 Legacy 冻结 | [01_LEGACY_AND_PROBLEMS.md](01_LEGACY_AND_PROBLEMS.md) |
| 02 分层与依赖 | [02_LAYERS_AND_DEPENDENCIES.md](02_LAYERS_AND_DEPENDENCIES.md) |
| 03 Core/Transport | [03_CORE_AND_TRANSPORT.md](03_CORE_AND_TRANSPORT.md) |
| 04 Graph/Vulkan Foundation | [04_GRAPH_AND_VULKAN_FOUNDATION.md](04_GRAPH_AND_VULKAN_FOUNDATION.md) |
| 05 Backend/Runtime/Frame/Target | [05_BACKEND_RUNTIME_FRAME_TARGET.md](05_BACKEND_RUNTIME_FRAME_TARGET.md) |
| 06 Feature/Capability | [06_FEATURE_AND_CAPABILITY.md](06_FEATURE_AND_CAPABILITY.md) |
| 07 RenderSystem/Projection/ResourceDomain | [07_RENDER_SYSTEM_PROJECTION_RESOURCE.md](07_RENDER_SYSTEM_PROJECTION_RESOURCE.md) |
| 08 C++/RAII/性能 | [08_CPP_RAII_PERFORMANCE.md](08_CPP_RAII_PERFORMANCE.md) |
| 09 阶段路线 | [09_IMPLEMENTATION_PLAN.md](09_IMPLEMENTATION_PLAN.md) |
| 10 实施合同 | [10_LLM_CONTRACT.md](10_LLM_CONTRACT.md) |
| 11 R0 工作单 | [11_R0_WORK_ORDER.md](11_R0_WORK_ORDER.md) |

## V1 构建闭包与证据身份

[V1_SOURCE_AUDIT.json](V1_SOURCE_AUDIT.json) 保存 frozen SHA 的 31 份相关 CMake 原文/blob 及 280 条
通信候选检索命中；这些是 source inventory，不是执行追踪或新构建资格。
[V1_HISTORICAL_TARGETS.json](V1_HISTORICAL_TARGETS.json) 保存已核验 MA11 归档中的 11 个真实 target
File API 记录（依赖、源码、编译 include、link 和 install）。图中依赖方向是 A depends on B。
该历史图只属于 `7d84e8785ce6778e180d9f78cbb6e96bdd409fb5`，没有冒充 frozen SHA 的重新配置。

与历史图相比，冻结来源中 Root、function 总入口、Render codegen、features、feature_client、
ui_rendering 六份 CMake blob 已变化；逐项比较保存在 source audit，最终来源以 frozen CMake 为准。
R0 不借已有脏工作区 build tree、旧 DLL 或 installed SDK 补齐 V2。

| V1 target/入口 | frozen source 中的职责与主要依赖 |
|---|---|
| render_client | 公共协议、句柄、错误、packet/client；依赖 lux-cxx、math、serialization、error、description；安装通用 codegen 与模板 |
| render_feature_client | builtin Operation schema、typed client 与生成入口；依赖 render_client；TOOLCHAIN 也消费 |
| render_standard_content | ClassicMeshBatchCodec 与 cooked content；identity/compile_time，私有 serialization；TOOLCHAIN 也消费 |
| render_graph | DependencyAnalyzer 与逻辑图公开头 |
| render_vulkan | Vulkan/VMA、client、feature_client、graph、window；包含旧 backend/graph/resource/scene 实现 |
| render_features | packed-content=OFF 时仅内容提供入口；ON 时包含 shaders、assets、codegen 与真实 Vulkan feature 实现 |
| render_runtime | PUBLIC render_client，PRIVATE render_vulkan；backend worker 与 owner-side admission/completion |
| scene_render | feature_client/runtime、Scene/Camera、ECS、asset/process；反射 codegen；生成 Scene 类型信息 |
| ui_rendering | PUBLIC ui/render_client；PRIVATE imgui Vulkan backend、Vulkan、render_vulkan；UI operation codegen |
| engine_context_render | engine_context、render_runtime、scene_render，私有 log；装配 Runtime 与 RenderResources |
| project_plugin_rendering | project_plugins、scene_render；插件注册 |
| examples/render-plugin | 外部 Feature DLL、shader 编译、插件导出、GPU consumer；Root 安装该样例 |

Root、modules/function、engine/context、engine/scene、engine/project/plugins、engine/scene/plugins、
editor、UI、BuiltinPlugins 和 installed consumers 的引用都保留原样。
TOOLCHAIN 通过 function profile 显式需要 client/feature_client/standard_content；R1–R9 按词汇、
typed schema、cooked content 分别复建，不能因离线工具没有 Vulkan 就漏掉这条迁移链。

## 历史四配置基线

[V1_BASELINES.json](V1_BASELINES.json) 保存每次真实命令、退出码、日志路径/哈希、历史实现 SHA、
首次失败和后续修正结果。归档全部文件哈希已经逐一核对，但不把文件完整性等同于新运行通过。

共同历史实现：`7d84e8785ce6778e180d9f78cbb6e96bdd409fb5`；收据
`docs/editor-mechanism-ma11-verification.md`，收据提交
`40286bf1fdd687901c1dd45aa395cc96c3c48046`。

| 配置 | 历史结果（仅上述实现） | frozen SHA 上本轮运行 |
|---|---|---|
| TOOLCHAIN | all/无新增工作，83/83 CTest | NOT_RUN |
| EDITOR | all/无新增工作，171/171 CTest，含 40 GPU、8 desktop | NOT_RUN |
| PLAYER | all/无新增工作，97/97 CTest | NOT_RUN |
| MSVC-ASan | all/无新增工作，130/130 适用 CTest | NOT_RUN |

历史 Windows 工具链为 MSVC 14.44.35207 x64、Ninja、C++20、RelWithDebInfo；vcpkg toolchain：
`D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake`。
依赖源码/SDK 身份为 lux-cxx `0a0e7419fc7229df6e372cd35a540249f92250ef`，当时前缀为
`E:/SyncForder/CodeRepos/install/Framework-v2-dependencies`。当前同名目录不自动具有当时身份。
ASan 使用逐目标第一方插桩脚本，不是仅设置一个全局开关；原始命令与脚本纳入已核验归档。

主归档 `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma11/evidence`：
manifest `aeb2e53a43613362c6b97b44c7f39224a2ace5d3197d4690df33e0c9fdf58ead`，5818 个文件。
其中根 `qualification-results.json` 保留首次 TOOLCHAIN configure 失败；最终四配置属于
`r1/qualification-results.json` 与 `runs/commands.json` 的 `ma11-r1-cold-*`，不可混用。
本轮还核对 UBSan 138、失败分类 119、MA07 Actions 94 个归档文件；已知限制见下一文件。
无新的 V1 GPU、SDK、性能或 sanitizer 运行，缺少 frozen SHA 新性能基线不填虚构数字。

## 恢复与未来复跑

恢复源码时优先使用已有 oracle；另建目录的等价命令：

```powershell
git worktree add --detach <new-v1-directory> a669409a289a6fa4092f21176397795b1cdb7f3e
git show a669409a289a6fa4092f21176397795b1cdb7f3e:<original-path>
```

`git show` 形式用于定位原内容；二进制恢复使用 Git checkout 或二进制安全读取，不经文本管道重写。
完整资格仍须先 ValidateTrackedSnapshot，再从固定提交创建独立 clean clone；oracle 不挂回 V2。
未来获准复跑时，按 `V1_BASELINES.json` 的各 profile 原始 command 数组重建：只替换 source/build/install
为新的隔离路径，核实依赖 SDK 身份；ASan 的 `CMAKE_PROJECT_lux-engine_INCLUDE` 指向同一归档内
`r1/instrument-first-party.cmake`。先启用 x64 VS 环境，运行配置、两轮 `all -j 4 -- -k 0` 与
`ctest --output-on-failure --no-tests=error -j 1`，构建/实机串行。该入口本轮未执行。

R0 bootstrap 的配置与检查方法：

```powershell
cmake -DLUX_SOURCE_DIR=<clean-I> -P <clean-I>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clean-I>/cmake/render-v2-bootstrap -B <isolated-build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build <isolated-build> --target all -j 4 -- -k 0
cmake --build <isolated-build> --target all -j 4 -- -k 0
py -3.13 <clean-I>/cmake/render-v2-bootstrap/verify_r0.py --source <clean-I> --output <outside-source>/static.json
```

资格构建额外请求 CMake File API codemodel-v2/cmakeFiles-v1/toolchains-v1，并向检查脚本提供
`--bootstrap-build <isolated-build>`；空项目的 CTest utility target 不算 Render 生产 target。

## 历史文档处理与后续边界

本轮历史文档实际删除集合：`[]`。47 份 `docs/editor-mechanism-*` 原文/原判定全部保留，
不删除树外证据、不改 Git history；无需为不存在的删除伪造恢复清单。
旧公共头未作内容修改，也不运行安装或污染 Debug/RelWithDebInfo/Android 开发 SDK。
R1 新公共头开始遵守三前缀同步要求，Android 构建仍不在默认矩阵。
本轮只允许 R0 完成后停止；下一阶段唯一允许建设的层是 render_core。
