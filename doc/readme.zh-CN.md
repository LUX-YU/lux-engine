# LUX-ENGINE

分层式实验 / 学习向游戏引擎（Rendering & Software Architecture Playground）。

目标：在一个清晰的"层 -> 组件 -> 功能"结构中，循序渐进地探索现代游戏引擎/实时渲染/工具链相关技术，包括：平台抽象、事件与反射系统、资源与序列化、ECS/脚本、渲染管线（前向 / 延迟）、可视化编辑与流程编排（FlowForge）、以及未来的物理、动画、AI 等模块。当前仍处于非常早期（Basic Version Dev）阶段，许多功能骨架已搭好但实现不完整。欢迎用作学习或二次开发实验场。

> [English version](./readme.md)

---

## 1. 架构概览

顶层以"Layer"划分逻辑域，每个 Layer 下再拆分为若干 Component（以 CMake component 形式组织并可被其他层声明式依赖）。

运行时分层（`modules/`）:
1. platform: 最底层平台与图形 API 抽象（common, gapi, window, dynamic_library）
2. core: 基础能力（math, meta(反射), script）
3. resource: 资源系统（description, asset：模型/纹理/材质/Shader/脚本 等资产与序列化）
4. function: 上层功能逻辑（render, flowforge, gameplay, ui）

其中 `function::ui` 是一个**可复用的 ImGui UI 框架**——只依赖 imgui + render + window + meta，
可单独链接用于开发独立 UI 程序，不会拖入 flowforge / script / asset / MLIR。

编辑器层（`engine/`）: lux-engine 编辑器，构建在运行时分层之上。它**不是** `modules/` 分层——
`engine/` 依赖 `modules/*`，反向永不依赖。
* **`engine::editor`** — 引擎耦合的编辑器面板（节点图编辑器、Lua 控制台、资产浏览器）+ 外壳
* **`engine::flowforge_compiler`** — FlowForge → MLIR → LLVM 离线编译器；经 `LUX_ENABLE_FLOWFORGE_MLIR` 选择性开启；全引擎唯一链接 MLIR/LLVM 的目标
* **`asset_pipeline`** — `lux_asset_packer` 构建期 CLI 工具（只链 `resource::asset`；因运行时构建本身依赖它，故无条件构建）

`-DLUX_BUILD_EDITOR=OFF` 可得到纯运行时构建（排除 `engine::editor` 与 `engine::flowforge_compiler`）。

渲染（function/render）当前包括：
* Vulkan 后端（通过 gapi + window）
* GLSL -> SPIR-V 的编译（glslc）集成
* 前向 / 延迟渲染管线的 Shader 目录与编译脚本骨架
* 资源层（asset）对接模型/材质/纹理加载，用于渲染资产转换（AssetConverter）

脚本（core/script）：
* LuaJIT 集成（LuaJIT::LuaJIT）
* MLIR 相关库已链接（IR, Dialect, Parser, Pass 等），表明计划进行 DSL/中间表示实验

反射系统：
* 依赖自建 lux-cxx（compile_time / static_reflection / dynamic_reflection_runtime / generator），用于元数据/序列化/编辑器交互（仍在打通）

FlowForge（function/flowforge）：
* 目前可见 IR 节点类声明，趋向于一个"可视化流程/节点图"系统（仍未完善，实现少）

---

## 2. 外部依赖 (Direct find_package / 查找逻辑)

必备工具链:
* CMake >= 3.22
* C++20 编译器 (Clang / GCC / MSVC)
* Ninja (推荐) 或 Make

第三方库:
* Vulkan SDK (含 glslc) — 渲染 & Shader 编译
* GLFW3 — 窗口与输入
* Eigen3 — 数学库
* Assimp — 模型导入
* stb (header only, 用于图像加载，要求系统头路径或手工 vendor)
* stduuid — UUID 生成
* LuaJIT — 脚本运行时
* MLIR (LLVM 项目的一部分) — 计划进行 IR/编译层实验
* fmt (可选，CheckFormatSupport.cmake 中 QUIET 查找)
* PkgConfig (辅助查找 LuaJIT/格式化工具)

项目内子项目 / 私有依赖:
* lux-cmake-toolset — 构建辅助（generate_visibility_header, add_component 等宏）
* lux-cxx — 自研 C++ 元编程 / 反射 / 代码生成工具链 (compile_time / static_reflection / dynamic_reflection_generator / dynamic_reflection_runtime)

注意：lux-cxx 与 lux-cmake-toolset 需要在本机已安装 (CONFIG 模式 find_package)。若未安装，需要先从其各自仓库构建 & install，或改写为 add_subdirectory 的本地依赖。

---

## 3. 目录简述

```
modules/       (运行时分层 — 发行版产品可链接的部分)
  platform/    (common, gapi, window, dynamic_library)
  core/        (math, meta, script)
  resource/    (description, asset)
  function/    (render, flowforge, gameplay, ui)
engine/        (编辑器层 — 依赖 modules/*，反向永不依赖)
  editor/             (引擎耦合的编辑器面板 + 外壳)
  flowforge_compiler/ (FlowForge MLIR/LLVM 编译器；选择性开启)
  asset_pipeline/     (lux_asset_packer 构建期工具)
spir_v/        (编译生成的 SPIR-V Shader 输出)
cmake/         (自定义 Find*.cmake & 辅助脚本)
```

---

## 4. 构建步骤 (Linux 示例)

确保安装：Vulkan SDK、glfw3、Eigen3、Assimp、LuaJIT、MLIR、stduuid、pkg-config、(fmt 可选)。不同发行版包名可能略有差异，MLIR 通常需要从 LLVM 源码构建（开启 -DMLIR_ENABLE_BINDINGS=ON 等）。

示例（概念性，仅供参考，需按发行版调整）：
```bash
# 1. 准备依赖 (示例 Ubuntu - 实际包名/版本可能不同)
sudo apt install build-essential ninja-build cmake pkg-config \
    libvulkan-dev vulkan-tools glslang-tools \
    libglfw3-dev libeigen3-dev libassimp-dev \
    liblua5.1-0-dev # (如果使用系统 Lua, 但这里需要 LuaJIT, 见下)

# LuaJIT
sudo apt install luajit libluajit-5.1-dev

# stduuid (可能通过包管理或 vcpkg / 手动安装)
# fmt (可选)

# MLIR: 通常自行构建并安装 (略)。确保提供 MLIRConfig.cmake

# 2. 克隆并安装 lux-cxx / lux-cmake-toolset (如果尚未安装)
# git clone ... && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --target install

# 3. 构建本项目
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 4. （可选）安装
cmake --install build --prefix /your/install/prefix
```

Windows / macOS: 同理，但需使用对应 Vulkan SDK、GLFW 安装方式。MLIR 构建成本较高，如不需要脚本 IR 功能，可暂时在 CMake 中注释 script 组件依赖以便快速上手。

---

## 5. 运行 / 快速验证

当前仓库尚未暴露统一的可执行 Demo（可能计划在 tools/ui 或 function/render 测试中）。可查看 `modules/function/render/test` / 相关开关 (ENABLE_RENDER_TEST / ENABLE_SCRIPT_TEST) 开启后生成测试目标。

示例启用渲染测试：
```bash
cmake -B build -G Ninja -DENABLE_RENDER_TEST=ON
cmake --build build
```

Shader 编译：构建时自动调用 glslc 生成 SPIR-V，输出至 `spir_v/` 或 CMake 的指定生成目录（`${SHADER_COMPILE_OUTPUT_PATH}`）。

---

## 6. 当前完成度评估

| 模块 | 状态 | 备注 |
|------|------|------|
| platform::window | 基础窗口创建 + Vulkan 标志 | 输入/多平台适配不完整 |
| platform::gapi | Vulkan 标志/宏定义封装 | 缺抽象层接口 & 后端切换机制 |
| platform::event | 事件接口层 | 缺实际事件分发实现 |
| core::math | 接口 + Eigen | 需补矩阵/向量/变换封装与 SIMD 优化 |
| core::meta | （未在本次片段展示） | 反射/注册机制需与资产、脚本、编辑器贯通 |
| core::script | LuaJIT + MLIR 链接 | 缺 VM 管理、脚本绑定、热重载、MLIR pipeline |
| core::ecs | 仅引用出现在依赖字符串 | 尚未实现：ECS 框架/调度 |
| resource::asset | 资产类型 & 序列化骨架 | 缺引用计数、异步加载、缓存、依赖图 |
| function::render | 管线骨架 + Shader 编译 | 缺帧图/渲染队列/材质系统/延迟光照实现细节 |
| function::flowforge | IR 基础头文件 | 缺节点系统 UI / 执行调度 / 序列化 |
| function::ui | ImGui UI 框架（面板、docking、控件、ImGui<->渲染桥接） | 可复用的运行时层组件，可单独链接 |
| engine::editor | 引擎耦合的编辑器面板（节点编辑器、Lua 控制台、资产浏览器） | 基于 ImGui；受 LUX_BUILD_EDITOR 门控 |

整体：核心 runtime（ECS, 资源加载管线, 渲染 FrameGraph, 反射编辑链路）尚未完成；目前更像是架构蓝图 + 部分底层钩子。

---

## 7. Roadmap / TODO

短期 (基础打牢):
1. 完成 core::ecs 模块（实体/组件存储、系统调度、事件桥接）。
2. 扩展 event：统一事件总线、输入系统（键鼠/窗口/自定义），与 ECS System 对接。
3. 渲染：建立抽象 FrameGraph / RenderGraph；补全前向 + 延迟通道，添加 GBuffer、Lighting Pass 真实实现。支持 Descriptor 管理、材质参数系统。
4. 资源管线：引入异步加载（线程池 + Job System），资源引用计数 / 生命周期 / 资产依赖追踪，缓存策略（LRU）。
5. 反射(meta) 与资产/脚本/编辑器联动：统一类型注册，支持属性编辑、序列化、Inspector 面板。
6. 脚本：封装 LuaJIT VM 多实例管理，ECS 组件/系统绑定；MLIR 方向明确（是做脚本 JIT 优化还是 DSL）。

中期 (功能扩展):
7. 编辑器 (`engine::editor` + `function::ui`)：ImGui Docking + 面板系统（场景层级、资源浏览、属性、日志、渲染调试、FlowForge 节点编辑）。
8. FlowForge：节点定义（反射驱动）-> 可视化 -> 序列化 -> 运行时执行（解释 / 编译 / JIT）。
9. 渲染增强：PBR 材质、IBL、阴影、后处理（Bloom, TAA, ToneMapping），GPU Profiler。
10. 多平台：Windows / Linux 初步统一，后续 macOS（Metal 抽象层？或保持 Vulkan via MoltenVK）。
11. 资源格式导出工具链（离线 Pipeline：模型预处理、纹理压缩、Shader 反射生成）。

远期 (高级特性):
12. 物理（Bullet/PhysX 或 自研）、动画系统（骨骼、混合树）、Audio、AI 行为树。
13. 热重载（资源 & 脚本 & Shader），增量构建资产。
14. 多线程任务系统（Job System）驱动渲染与资源加载并行。
15. ECS + 渲染 FrameGraph + 资源 Streaming 的统一调度策略。
16. 插件系统 / 模块化运行时装载。
17. 网络（可选）与多人同步架构实验。

工程化 / 质量改进:
18. 引入单元测试/集成测试（当前 test 目录启用条件较多）。
19. 静态分析 (clang-tidy), 覆盖率, Sanitizers, CI 脚本。
20. 文档与示例：最小 Demo、脚本示例、节点图示例。

---

## 8. 贡献建议

* 先聚焦某一 Layer：例如先完成 ECS 或 FrameGraph，避免多线发散。
* 保持组件间依赖自下而上（platform -> core -> resource -> function）；`engine/` 编辑器层位于其上、依赖 `modules/*`，反向永不依赖，禁止逆向耦合。
* 为每个组件补充：设计说明（README 子节）、最小示例（tests 或 examples）、对外头文件清单。
* Shader / 脚本生成步骤写入文档 & 可复现脚本（避免"构建后才知道输出路径"）。

---

## 9. License

（未声明）建议尽早选择（MIT / Apache-2.0 / GPL 等）以便外部使用者明确合规性。

---

## 10. 状态声明

本仓库为个人/学习驱动的早期探索项目，API / 目录结构随时可能重构。请谨慎用于生产。

---

欢迎 Issue / PR / 讨论。若你也在做学习型引擎，互相交流演进路径或基准测试经验会很有价值。

（本 README 中文版基于当前仓库与 CMake 配置整理，若后续结构有较大调整请同步更新。）
