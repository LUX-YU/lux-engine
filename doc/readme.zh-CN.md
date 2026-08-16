# lux-engine

`lux-engine` 是一个现代 C++20 游戏引擎项目，重点是显式所有权、确定性的 ECS
调度、异步资产流送与 Vulkan 渲染。仓库同时按**职责**与**发行产品**分层：Runtime
不能依赖 Authoring、Toolchain 或 Editor。

> [English](../README.md) · [当前唯一活待办](../.internal/UNFINISHED-WORK.md)

## 架构

受 CMake 门禁约束的依赖主干为：

```text
Platform -> Core -> Resource -> Function -> ECS -> Runtime -> Host
                         \-> Authoring -> Toolchain -> Editor
                                      Runtime ----^       ^
```

- `modules/platform`：OS、窗口、动态库、文件通知和图形 API 入口。
- `modules/core`：无游戏领域语义的 bytes、serialization、events、log、math、meta
  与 extension ABI。
- `modules/resource`：cooked runtime 资源描述、资产身份/账本、运行时 codec 与 pak reader。
- `modules/function`：不认识 EnTT entity 的 render、script、input、UI、FlowForge 等功能。
- `ecs`：Component、`ISystem`、`IRenderSubsystem` 和 ECS 领域适配。
- `engine/runtime`：execution、assets、scene、render、extensions、frame 与 runtime packs。
- `engine/authoring`：可编辑的源文档和项目数据。
- `engine/toolchain`：authoring→cooked 转换；Assimp、shaderc、SPIR-V reflection、
  MLIR/LLVM 只允许出现在这里。
- `engine/editor`：仅编辑器 controller、panel 与 framework。
- `engine/hosts`：composition root、主循环和显式关停偏序。
- `extensions`：可部署的 `MODULE`；引擎 target 不反向链接具体扩展。

每个 production target 声明 `LAYER`、`PRODUCT` 和 `ROLE`。非法层依赖、非法产品
依赖、未分类 production target、引擎反向链接具体 Extension 都会在配置期失败。

## 运行时模型

- Scene 拥有 `World`、已拓扑编译的 `Schedule`，并且至多有一个顶层
  `RenderSystem`；不安装 render pack 即为 headless。
- Frame、Control、持久 GPU Upload 使用三条独立通道；单一 transfer pipeline
  负责传输录制/提交，不用 queue-submit mutex。
- `AsyncRuntime` 使用注册方拥有的强类型有界队列、单一 standalone-Asio coordinator、
  小型 BlockingIO 兼容 executor 与 oneTBB CPU 后端。
- `MainThreadMailbox` 是异步 completion 回到 ECS、`AssetManager`、UI 与
  `DomainEvents` 的唯一入口。
- `DomainEvents` 只广播已经提交的事实，不承载命令或请求/回复。
- 动态扩展拆为 Module、Contribution、Activation；类型身份不使用 RTTI。

引擎代码禁止 RTTI 和异常控制流。失败使用 `expected`，跨线程载荷必须拥有数据，
热路径避免锁、无界队列和隐式等待。

## Render 四层

- `lux::engine::function::render_client`：后端无关 handle、协议、channel 和
  Frame/Control/Upload session；公共头不出现 Vulkan 类型。
- `lux::engine::function::render_graph`：逻辑资源、pass、依赖分析与逻辑执行计划；
  无需 Vulkan device 即可测试。
- `lux::engine::function::render_vulkan`：Vulkan server、资源管理、graph lowering、
  queue 与 `GpuTransferPipeline`。
- `lux::engine::function::render_features`：内建 Vulkan feature 与生成资产。

ECS extraction 与 `FrameCoordinator` 只依赖 `render_client`；headless
`SceneRuntime` 不链接 Vulkan backend。

## 构建 Profile

使用 `LUX_BUILD_PROFILE`；已删除的 `LUX_BUILD_EDITOR` 不再支持。

| Profile | 内容 |
|---|---|
| `DEVELOPER` | Runtime、Player、Editor、Toolchain |
| `PLAYER` | Runtime 与参考 Player；可原生或交叉编译 |
| `EDITOR` | Runtime、Editor、Toolchain；不含参考 Player executable |
| `TOOLCHAIN` | 离线工具及其最小依赖 |

目标平台由 CMake toolchain 决定。例如 Android 使用
`LUX_BUILD_PROFILE=PLAYER`、Android triplet 和显式的
`LUX_HOST_TOOLS_PREFIX`；平台名不再作为产品 Profile。

```powershell
cmake -S . -B ../build/RelWithDebInfo/lux-engine -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DLUX_BUILD_PROFILE=DEVELOPER `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build ../build/RelWithDebInfo/lux-engine --target all -j 4 -k 0
ctest --test-dir ../build/RelWithDebInfo/lux-engine --output-on-failure
```

`lux-cmake-toolset`、`lux-cxx`、`imgui`、`imgui-node-editor` 需要通过
`CMAKE_PREFIX_PATH` 可见；`bootstrap/` 提供从上游开始的构建图。

修改 CMake 后必须构建两轮，第二轮应无工作。构建不能和实机/attended 验证并发。
修改 `modules/*` 公共头后要同步 Debug、RelWithDebInfo、Android 三个 install prefix，
因为 meta generation 读取安装树中的头。

## 产品与导出

主要 executable：

```text
lux_player
lux_editor
lux_launcher
lux_asset_packer
lux_shader_emitter
lux_game_exporter
```

`lux_game_exporter` 输出 cooked Player、runtime manifest、pak、runtime DLL 与 runtime
extension。Player 不再支持 loose authoring/project 内容；导出发现 authoring-only payload
或 Editor/Toolchain 二进制会失败。

安装与 CPack 区分可复用层和产品，包括 `lux_runtime`、`lux_player`、`lux_editor`、
`lux_toolchain`、`lux_sdk`。

## 当前状态

目录/target 大迁移仍在收口。产品 profile、依赖分类、Runtime/Editor host 拆分、Render
四 target、Asset identity/core/codecs/pak、Authoring/Toolchain、Extension module 与
Player inventory/export 门禁已经落地；尚未完成的物理目录拆分与平台验证只以
[`UNFINISHED-WORK.md`](../.internal/UNFINISHED-WORK.md) 为准。

## License

见仓库 License 文件。
