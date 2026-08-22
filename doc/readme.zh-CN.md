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

源码所有权主干为 `modules/ -> ecs/ -> engine/ -> hosts/products`：

- `modules/platform`：OS、窗口、动态库、文件通知和图形 API 入口。
- `modules/core`：无游戏领域语义的 bytes、serialization、events、log、math、meta
  与窄类型异步端口。
- `modules/resource`：cooked runtime 资源描述、资产身份/账本、运行时 codec 与 pak reader。
- `modules/function`：不认识 EnTT entity 的 render、script、input、UI、FlowForge 等功能。
- `ecs`：全部 Component、所有知道 Entity/Component 的领域行为、`ISystem`、唯一
  `Schedule`，以及 World 到 renderer 的 extraction。
- `engine/runtime`：execution、assets、Scene/World 生命周期、extension loader、render
  session/backend、frame 与 logging；不定义领域 Component 或 System。
- `engine/extensions/api`：极薄的 Plugin ↔ Engine 共享 ABI 契约。
- `engine/authoring`：可编辑的源文档和项目数据。
- `engine/toolchain`：authoring→cooked 转换；Assimp、shaderc、SPIR-V reflection、
  MLIR/LLVM 只允许出现在这里。
- `engine/editor`：仅编辑器 controller、panel 与 framework。
- `engine/hosts`：composition root、主循环和显式关停偏序。
- `extensions`：可部署的 `MODULE`；引擎 target 不反向链接具体扩展。

每个 production target 声明 `LAYER`、`PRODUCT` 和 `ROLE`。非法层依赖、非法产品
依赖、未分类 production target、引擎反向链接具体 Extension 都会在配置期失败；
固定的 `lux_architecture_check` Target 还会随 `all` 复检源码边界和退役词汇。

## 运行时模型

- Scene 拥有 `World`、已拓扑编译的 `Schedule`，并且至多有一个顶层
  `RenderSystem`；不安装 `RenderSystem` 即为 headless。
- Frame、Control、持久 GPU Upload 使用三条独立通道；单一 transfer pipeline
  负责传输录制/提交，不用 queue-submit mutex。
- ECS System 只接收窄类型 `OperationPort<T>`；Runtime 持有其 Asio/TBB/stdexec/
  concurrentqueue endpoint 实现和线程生命周期。
- `MainThreadMailbox` 是异步 completion 回到 ECS、`AssetManager`、UI 与
  `DomainEvents` 的唯一入口。
- `DomainEvents` 只广播已经提交的事实，不承载命令或请求/回复。
- ABI v5 Extension 是强制 fingerprint 的同工具链模块；Loader 将直接的 System/
  RenderFeature/panel 装配和 metadata batch 绑定到 `ModuleLease`，不存在 Contribution
  或 Activation framework。
- `SceneDescription` 只保存 cooked Entity/Component 数据；Component schema、所需
  Extension 与 renderer capability 均由 Cook 推导。

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
`LUX_HOST_TOOLS_PREFIX`；平台名不再作为产品 Profile。Windows 宿主工具前缀的
`bin/` 必须包含 `lux_meta_generator` 的完整运行时闭包：`libclang.dll`、
`zlib1.dll` 和 `zstd.dll`。Android 还要求设置 `VULKAN_SDK`：编译使用其中当前的
平台中立 Vulkan 头，目标端仍链接 NDK 的 Vulkan loader。

`LUX_SCRIPT_HAS_LUA` 只控制可选 LuaJIT backend。桌面默认开启；Android 在 triplet
提供目标端 LuaJIT 包之前默认关闭。关闭后，backend-neutral ScriptSystem 与 native
script backend 仍然存在。

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

语义去重迁移已经落地：Contribution/Pack 身份、第二套 Render 图、Component 双注册和
Runtime 所有的 ECS 领域均已删除。剩余的平台/Profile 验证证据只以
[`UNFINISHED-WORK.md`](../.internal/UNFINISHED-WORK.md) 为准。

## License

见仓库 License 文件。
