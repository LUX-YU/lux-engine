# F0 — 输入来源、历史合同与保护基线

## 来源

| 输入 | 身份与用途 |
| --- | --- |
| FINAL ZIP | `LuxEngine_RenderV2_FINAL_Architecture_2026-10-10.zip`，唯一新目标规范 |
| ZIP SHA-256 | `39be1dd08ca4e30964ef47fa0b274054bc426e79472a1d0e733deaa0fe83d551` |
| 内部 manifest | [final/MANIFEST.sha256](final/MANIFEST.sha256)，43 项覆盖除 manifest 自身外的全部文件 |
| FINAL baseline | `7de3ddaa3f7614995745b41d73dfc98302310a4c`，本轮开工重新核实远端一致 |
| Frozen V1 | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| RD0 输入 | 用户单独提供的合订 MD 和 66 行 CSV；仅外部证据归档，不作为第二执行入口 |

原包 20 章、四张表 86/131/51/25 行均原样保留。
旧 F01–F36/I01–I30 的 66 个编号及 SourceFile/OperationSchema/SourceSHA 与 RD0 一致。
其中 102 处非空源码/操作引用按冻结 tree 核对；`render_legacy/` 前缀对应冻结提交原路径，
其余中立 Shader 文件直接对应冻结提交的同名路径。新增 I31–I44/H01–H06 是设计要求，不是已迁移能力。

## 旧规范及已批准修订映射

R2 原 00–11 的逐章迁移使用 [FINAL 18 §18.2](final/18_历史规范迁移.md)。
`_references_R2_readonly` 只保存原设计文本，不能覆盖仓库后来批准的 R2-FIX 修订。

| 当前仓库合同 | FINAL 对应位置 | 保留的具体不变量 |
| --- | --- | --- |
| 03_CORE_AND_TRANSPORT.md §13 | 03 §3.1/3.8，13 §13.2/13.4，19 D08/D09 | pre-reserved reply、consume/abandon、WRITING 写者回收；不恢复 pending retry |
| 05_BACKEND_RUNTIME_FRAME_TARGET.md §18 | 08 §8.4/8.8，13 §13.3/13.4，19 D32 | 无新 PROGRAM 仍可 paced frame；GPU completion 与 frame serial 不同 |
| 07_RENDER_SYSTEM_PROJECTION_RESOURCE.md §21 | 03 §3.3–3.6，08 §8.4，19 D07/D10/D32 | Simulation/Publication/Rendering 分责；retained revision 与共享 CPU asset domain |
| 09_IMPLEMENTATION_PLAN.md 的 R3/R5/R7/R8 门禁 | 15、16，19 D31/D32 | 原义务保留，未来顺序变为 F0–F11/H1–H6；机制与集成证据分开 |
| 10_LLM_CONTRACT.md 的独立进度 HARD GATE | 17 §17.3，19 D01/D32 | 单 Runtime/Backend/FrameLoop，无强制 tick→packet→frame |
| R4 native owner 与 frame lifetime 既有合同 | 02 §2.4，07 §7.1/7.3/7.9，13 §13.4/13.5，19 D24/D28 | 保留 R4 owners；真实 in-flight/fence 退休，不以 PROGRAM 消费或帧算术销毁 backing |
| 旧 View/Graph、R3 简化 API 和旧 R17 cutover 目标 | 04 §4.6/4.7，06 §6.7，08 §8.2，15 §15.5，18 §18.3 | Backend 共享计划；View 不默认 Graph owner；R3 可重写；新 F11 才产品切换 |

历史报告全部保留：R2 原始 PARTIAL 不被 R2-FIX PASS 覆写；R3 CPU 范围和 R4 单 GPU/queue 范围不扩大。
`V1_KNOWN_ISSUES.md` 的 transfer_idle、skinning WAR、Clang/UBSan、Linux、输入/IME 等原事实不改。
原包合订版只读，既有历史文档删除集合为空。

## 实施父提交的受保护 tree

| 路径 | Git tree ID |
| --- | --- |
| `modules/function/render/core` | `07e3fa1204d6bcc0a7e28e692619ee402cc1f6f4` |
| `modules/function/render/transport` | `b09b2afbd383b0ac692d8e1f667d911cb2c9af8b` |
| `modules/function/render/graph` | `c0fbb831ce5c3a661e5b33b04f89b8ef48e500e9` |
| `modules/function/render/vulkan` | `1bfa7147b83fbed7585cc5b3ecd432cc865614ec` |
| `render_legacy` | `2b7d13c91c07969c997534de7d21143bd8e440a2` |

独立验收还比较允许范围外的全部 tracked 路径，而不只比较上述五个子树。
Legacy 719 文件依据 `LEGACY_SOURCE_MANIFEST.json` 检查 mode/blob/size；整个 archive 720 项含防误用 CMake。

## 用户工作区与外部证据

原工作区 `E:/SyncForder/CodeRepos/lux-engine` 在 `codex/editor-framework-v2`，HEAD 为 Frozen V1。
六处保护修改：

- `editor/app/include/lux/engine/editor/LuxEngine.hpp`
- `editor/context/include/lux/engine/editor/EditorContext.hpp`
- `modules/function/render/features/src/assembly/meshstack/MeshStackOperationHandlers.cpp`
- `modules/function/render/vulkan/sinclude/lux/engine/render/resources/mesh/InstanceResources.hpp`
- `modules/function/ui/CMakeLists.txt`
- `modules/function/ui/include/lux/engine/ui/Pane.hpp`

开工快照：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F0/preflight-7de3ddaa3f76/`。
其中保存两个 worktree 的 HEAD/branch/status、binary staged/unstaged diff、untracked 清单、Git tree、配置、
六文件原字节/哈希、原 ZIP/RD0 输入和命令退出码。最终资格归档按 implementation SHA 放入同级目录。
用户如自行修改，记录新旧差异并重建保护基线，不恢复覆盖用户文件。

本轮不修改生产头，install 前缀不变；构建、CTest、GPU、Sanitizer、性能、SDK/产品及跨平台资格均 NOT_RUN。
