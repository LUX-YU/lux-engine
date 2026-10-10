# 11 — Render V2 R0 正式实施工作单（交付 LLM 使用）

> **唯一授权阶段：R0 — Legacy Freeze / Isolated Build Bootstrap。R1–R18 均未授权。**
>
> 仓库：`LUX-YU/lux-engine`  
> 起点分支：`codex/editor-framework-v2`  
> 唯一批准 frozen V1 reference SHA：`a669409a289a6fa4092f21176397795b1cdb7f3e`  
> 目标新分支：`codex/render-v2`  
> 冻结状态：**REFERENCE_WITH_KNOWN_LIMITATIONS**，不是旧 MA 最终全项 PASS。  
> 日期：2026-10-10

## A. 必须先读取的权威规范

本 R2 ZIP 必须随本工作单一起提供给实施 LLM。**第一次执行时仓库内尚没有 `docs/render-v2/`，应先从输入附件读取本工作单及 R2 文档，按映射导入新 worktree，再从仓库路径复读以核对。** 若未收到完整规范/manifest，STOP，不从历史聊天或旧 R1 猜测。

先读（如尚未入库，则从 R2 附件读取相应原文件）：

```text
docs/render-v2/00_README.md
docs/render-v2/01_LEGACY_AND_PROBLEMS.md
docs/render-v2/02_LAYERS_AND_DEPENDENCIES.md
docs/render-v2/08_CPP_RAII_PERFORMANCE.md
docs/render-v2/09_IMPLEMENTATION_PLAN.md
docs/render-v2/10_LLM_CONTRACT.md
本 R0 工作单
```

R0 不必逐行加载 Feature、Graph、Projection 的实现文档；它们属于后续阶段。要迁移算法时才读取对应规范与 V1 源码。

> **文档落库命名规则**：R2 交付包里的 `00_...md`–`10_...md` 需按上面的稳定英文文件名映射一一落入 `docs/render-v2/`；**不保留两套同时宣称权威的重复规范**。保留正文原文，变更记录只列出路径映射，不对正文自行“顺手改进”。本工作单保存为 `docs/render-v2/11_R0_WORK_ORDER.md`。

### 文件名映射

```text
00_README_总览与文档索引.md              → 00_README.md
01_现状问题与Legacy冻结策略.md           → 01_LEGACY_AND_PROBLEMS.md
02_分层架构与依赖规则.md                 → 02_LAYERS_AND_DEPENDENCIES.md
03_Core与Transport_数据契约和通信.md    → 03_CORE_AND_TRANSPORT.md
04_RenderGraph与VulkanFoundation.md    → 04_GRAPH_AND_VULKAN_FOUNDATION.md
05_VulkanBackend_Runtime_Frame_Target.md → 05_BACKEND_RUNTIME_FRAME_TARGET.md
06_Feature系统与SceneCapability.md       → 06_FEATURE_AND_CAPABILITY.md
07_RenderSystem_Projection_ResourceDomain.md → 07_RENDER_SYSTEM_PROJECTION_RESOURCE.md
08_Cpp_RAII_错误语义与热路径规范.md        → 08_CPP_RAII_PERFORMANCE.md
09_实施阶段_VerticalSlice_算法迁移.md   → 09_IMPLEMENTATION_PLAN.md
10_验收门禁与LLM实施合同.md              → 10_LLM_CONTRACT.md
11_R0_实施工作单.md                      → 11_R0_WORK_ORDER.md
```

## B. 开始之前必须满足的事实检查

1. 从远端读取 `codex/editor-framework-v2` 的 HEAD；若与 `a669409a289a6fa4092f21176397795b1cdb7f3e` 不同，不得自行选择新 SHA 或合并。停止并报告。
2. 当前工作区若有用户未提交修改，禁止覆盖、reset、stash-pop 或混入 R0。优先新建**干净独立 worktree** 实施 `codex/render-v2`。用户原 worktree 原样保留。
3. 记录 V1 当前 CMake 真实目标/依赖 closure、源目录、source manifest（文件路径、大小、Git blob SHA），以及最少四种配置的基线（`TOOLCHAIN / EDITOR / PLAYER / MSVC-ASan`，只引用已有可证明收据；无法运行标 NOT_RUN）。
4. 不把 V1 的 UBSan/LINUX/Render known issues 强行修成 PASS。用户已经允许后续修复。

## C. 允许修改范围

```text
ALLOWED:
    docs/render-v2/**
    render_legacy/**
    cmake/render-v2-bootstrap/**
    仅限将列出的旧 Render closure 原样 git mv 出的源路径
    经精确逐文件判别的历史中间 verification 文档

READ-ONLY REFERENCE (迁出前):
    modules/function/render/**
    engine/context/rendering/**
    engine/scene/builtin_systems/render/**
    modules/function/ui/rendering/**
    engine/project/plugins/rendering/**
    examples/render-plugin/**

FORBIDDEN:
    新 render_core/transport/graph/vulkan/runtime/features 生产 C++ 实现
    Render V1 bug fixes
    Editor / Flow / Material / Script / Scene 算法修改
    通用 service/manager/compatibility layer
    全局旧/新 Renderer 切换开关
    将 render_legacy 接回任意 V2 target
    修改 main、旧历史标签和已归档证据
```

`READ-ONLY REFERENCE` 指不允许**修改旧代码内容**；R0 仅允许将完整原字节与相对路径迁到冻结 archive，不能改其中的实现来“修 CMake”。

## D. 必须按此顺序实施

### R0.1 冻结 baseline

- 生成 `docs/render-v2/V1_KNOWN_ISSUES.md`，仅保留最简短的事实：`render.transfer_idle` 缺完成标记、skinning cross-frame WAR、历史 minimize、Clang/UBSan `100/130`（这是历史实测，未经修复）、Linux NOT_RUN、native input/IME postponed。每项包含来源文档 Git path/SHA；**不得声称根因均已确定**。
- 生成 `docs/render-v2/LEGACY_MANIFEST.md`：冻结 SHA、代码路径闭包、文件清单来源与哈希方案、恢复命令、V1 独立 worktree/工具链版本、R0 不覆盖已有用户差异。
- 把本次 R2 规范按映射导入 `docs/render-v2/`；以该包为唯一权威。旧 R1 附件不是仓库权威，不重复保存。

### R0.2 创建 V2 分支和 frozen Legacy source

- 从确切 `a669409a289a6fa4092f21176397795b1cdb7f3e` 建新分支 `codex/render-v2`。若该分支已存在但 HEAD 不等于明确的预期来源，**不得 reset/force**；停止并报告。
- 用 `git mv` 将这六个完整 closure 的**现有内容和相对层级**迁入仓库顶层 `render_legacy/`：

```text
modules/function/render/**              → render_legacy/modules/function/render/**
engine/context/rendering/**            → render_legacy/engine/context/rendering/**
engine/scene/builtin_systems/render/** → render_legacy/engine/scene/builtin_systems/render/**
modules/function/ui/rendering/**       → render_legacy/modules/function/ui/rendering/**
engine/project/plugins/rendering/**    → render_legacy/engine/project/plugins/rendering/**
examples/render-plugin/**              → render_legacy/examples/render-plugin/**
```

- `render_legacy/CMakeLists.txt` 只作 frozen source 防误用入口：

```cmake
message(FATAL_ERROR "Frozen Render V1 reference; build its pinned SHA in a separate worktree")
```

- Root 和 V2 模块不得 `add_subdirectory(render_legacy)`；新构建、安装、SDK、自动生成均不得包含 legacy 头/源/DLL。R0 不得添加旧 Render 的跳板 target 来让 Editor 假装仍可构建。

### R0.3 独立 bootstrap CMake（不写生产 Render）

在 `cmake/render-v2-bootstrap/CMakeLists.txt` 增加**唯一 standalone non-install CMake 入口**：

```cmake
cmake_minimum_required(VERSION 3.22)
project(lux-render-v2-bootstrap LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
# R0: no new production target exists yet.
# R1+: add only explicitly implemented V2 layer targets, never render_legacy.
```

R0 实际检查该空入口可完成 configure + generated Ninja/VisualStudio project。若需要在 CMake 内添加生产 target、把旧 Render 链回来、修改 unrelated root build graph 才能完成 R0，直接 STOP。

R1 将在该入口添加 `render_core` 的**真实** target/test。直到 R17 cutover 前，`codex/render-v2` 的全产品 EDITOR/PLAYER/TOOLCHAIN build 是已声明的 `EXPECTED_UNAVAILABLE`，**不是“测试通过”或可以填空 target 的理由**。V1 的全套产品测试只在独立 frozen V1 worktree 复核。

### R0.4 清理历史中间文档

- 只删可由 Git 历史恢复的**逐阶段实现/verification receipt**，优先 `docs/editor-mechanism-*` 中确为历史收据的文件。
- 不得删当前 SDK、module README、API contract、用户未提交文档、生成器使用的输入文档和与新 Render 实施直接有关的当前规范。
- 在 `docs/render-v2/LEGACY_MANIFEST.md` 记录**准确删除路径清单**、源 SHA、理由、`git show <SHA>:<path>` 恢复形式；先检查当前源码/工具是否引用它们。
- 不删除 Git 历史，不删除树外证据存档；R0 不改写历史报告结果。

### R0.5 静态核查与提交

- `render_legacy` 文件清单与原六个 closure 精确对照（可忽略 archive 自己新增的顶层 CMake 防误用入口）。逐文件字节或 Git blob SHA 一致；没有凭借复制丢漏隐藏文件/生成器模板/资产。
- Search tracked active V2 source/CMake/installed manifests：0 `render_legacy` includes/links/add_subdirectory；不能以隐式 include path 绕过。
- Bootstrap CMake configure PASS；冻结 V1 完整 build 仅在 V1 worktree 有条件验证，结果如实记录；V2 EDITOR/PLAYER/TOOLCHAIN 此时仍为 `EXPECTED_UNAVAILABLE`。
- 先 implementation commit，独立 verification，再 verification/docs commit，最后 **STOP，不进入 R1**。

## E. R0 验收证据

至少交付：

```text
R0 source/base SHA（不可含糊）
R0 implementation commit SHA
R0 verification commit SHA
frozen closure 原路径/新路径与 Git blob/hash totals
V1_KNOWN_ISSUES.md
LEGACY_MANIFEST.md
历史中间文档实际删除清单（或精确地说明未删）
独立 bootstrap CMake configure logs
V1 oracle worktree 的可用命令和已执行/未执行结果
V2 产品构建明确标 EXPECTED_UNAVAILABLE（不冒充 PASS）
用户现有差异未覆盖证明
0 new Render V2 production C++ source
```

## F. R0 禁止事项与 STOP

任一情况停止：

- Remote base SHA 改变，或目标 branch 与预期冲突；不得强推/重置。
- 用户工作区变更无法无损隔离。
- 闭包包含不能安全迁移的额外生产者，需修改 forbidden subsystem 才能完成。
- 只为取得绿色构建而新增 compatibility manager/adapter、fake target、旧 Render link、双 runtime switch。
- 历史文档是否为当前 contract 不能判定。
- Legacy 原始字节/文件清单无法核验。

R0 不负责性能优化、RAII 重写、Graph/Feature 算法迁移、transport 重构、GPU bug 修复。**这些工作只能在各自后续阶段发生。**

## G. 给下一阶段的唯一交接

R0 最终报告一段话：

```text
R0 PASS/FAIL/PARTIAL
V1 frozen SHA
V2 branch HEAD
legacy source manifest & known issues
bootstrap configure status
current V2 product status = EXPECTED_UNAVAILABLE
R1 allowed target = render_core only
STOP
```

不自动创建 R1 代码或开启下一阶段。
