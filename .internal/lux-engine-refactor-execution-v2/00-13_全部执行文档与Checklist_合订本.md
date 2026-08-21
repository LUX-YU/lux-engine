# LUX Engine 重构执行文档 v2 合订本

基线：GAPI 保留与单体 Input 施工基线 `d7f364d0`

> 本合订本由 00–13 号执行文档、详细施工 Checklist、迁移映射、代码事实索引、v2 修订说明、现行 ADR 及既有验收证据机械合并生成。独立文件是施工与评审的主版本；本文件用于全文检索和连续阅读。

## 文件顺序

- `00_架构宪章与变更控制.md`
- `01_modules公共SDK边界与分发体系重构.md`
- `02_Core与Platform基础库清理.md`
- `03_Resource-Description与Asset重构.md`
- `04_Function公共模块重构.md`
- `05_ECS内核-序列化与SceneFormat重构.md`
- `06_Engine执行-资产-场景与扩展重构.md`
- `07_Game-Editor与共享Session产品重构.md`
- `08_Editor-Workspace-Workbench-Documents-Panels重构.md`
- `09_CMake-命名空间-SDK包与兼容迁移.md`
- `10_测试-CI-PR路线与验收.md`
- `11_详细施工Checklist_已更新_20260819_b1a25d3.md`
- `12_迁移映射总表.md`
- `13_当前代码事实索引.md`
- `REVISION_NOTES_v2.md`
- `ADR-20260820_SceneAsset与Resource边界.md`
- `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`
- `ADR-20260821_Asset运行期需求与SerDeser边界.md`
- `ADR-20260821_CoreMeta纯化与ECSRegistry归位.md`
- `ADR-20260821_CoreSerialization与ECSComponentArchive边界.md`
- `ADR-20260821_ExtensionAbiV4Owner与Core清零.md`
- `ADR-20260821_GAPI保留裁决.md`
- `ADR-20260821_单体Input子系统边界.md`
- `evidence/asset-domain-cohesion-f35e245a.md`
- `evidence/asset-pipeline-core-meta-fe4422ba.md`
- `evidence/core-serialization-ecs-component-archive-6906ccc2.md`
- `evidence/extension-abi-core-retirement-2259ade7.md`

---

# LUX Engine 重构架构宪章与变更控制

> 定义不可变设计哲学、层级边界、命名规则、所有权规则与全套文档的执行顺序

**执行文档 00 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | 技术负责人、架构负责人、公共 SDK 维护者、ECS 与 Engine 维护者、编辑器负责人、代码评审者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** `ADR-20260820_SceneAsset与Resource边界.md` 是资产与场景边界的现行 SSOT。`modules/resource/asset` 作为可复用 SDK 保留 `LuxAsset`、`AssetManager`、`TAssetSerDeser`、Catalog、Provider、VFS 与 Pak；不再创建 `AssetId`、`AssetTypeId`、`engine/assets` 或第二套 `AssetStore`。Engine Scene 作为既有资产机制上的 Engine-owned 类型存在。本文后续与该 ADR 冲突的 AssetStore/新 ID 示例均已被取代。

> **2026-08-21 裁决更新：** 资产运行期只允许 `AssetClient -> AssetLoadService -> VFS -> Catalog -> SerDeser -> AssetManager`；AssetRef 不触发 IO。Core Meta 不拥有 EnTT、Registry 或 OO 标记根类，Registry 回归 ECS Core。详见 `ADR-20260821_Asset运行期需求与SerDeser边界.md` 与 `ADR-20260821_CoreMeta纯化与ECSRegistry归位.md`。

> **2026-08-21 Serialization 裁决：** Core Serialization 只保留 Archive/NameTable/Byte primitives；反射 Component Archive 归 ECS。UUID wire tag 不表达资产语义，资产引用必须使用显式 `asset_type=` annotation。不建立 Registry 文件镜像。详见 `ADR-20260821_CoreSerialization与ECSComponentArchive边界.md`。

> **2026-08-21 裁决更新：** `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md` 是 Asset 源码布局、Provider/VFS/Pak 存储边界与内置资产身份的现行 SSOT。Asset 按领域族组织；Pak 读写检查属公共 SDK；Engine 默认内容归 `engine/content`；ECS 通过装配参数接收 fallback ID。

> **2026-08-21 Extension ABI 裁决：** ABI v4 的实体定义归 `engine/extensions/api`，Core 不再安装 Extension API；namespace、descriptor 布局、ordinal、fingerprint、ABI-facing registrar 名称及导出 symbol 保持不变。通用 `ContributionId` 删除。ABI-facing 类型改名与显式 reflection codegen entry 留给独立 ABI 版本裁决。详见 `ADR-20260821_ExtensionAbiV4Owner与Core清零.md`。

> **2026-08-21 Extension ABI 实施状态：** `c56efbc4` 已完成上述裁决；Authoring 保持 source DTO，并在 Toolchain/Editor 边界显式转换为 Engine Extension API。四 Profile、动态 DLL、installed consumer 与旧 component 反向查找均通过。


## 0. 文档目的

本文件优先级高于其余施工文档。后续代码改动若与本文件冲突，必须先修改本文件并取得架构评审通过，不能在实现中自行“折中”。

当前仓库已经具备若干高质量机制：`ecs::Schedule` 对 `ISystem` 的唯一所有权、generation handle、拓扑编译、未发布事务、`ModuleLease` 风格的动态代码保活、类型化异步 Operation、主线程安全点和独立 Cook 后进入 Play。重构不应推翻这些机制，而应清除围绕它们形成的错误层级、重复包装与模糊命名。

本版替代此前全部 v1.x 执行文档。尤其废止以下旧建议：

- 不创建 `engine/module/include/lux/module/Modules.hpp`。
- 不把 `Extension`、CMake Library Module、`ScriptModule` 统一成同一种 Module。
- 不把 `AssetManager` 原样改名后继续放在公共 `modules/resource`。
- 不把 Scene、Deployment、Extension ABI、World Partition 等协议继续下沉到 Resource。
- 不把 `EngineRuntime` 作为 Game 与 Editor 的公共服务容器。
- 不允许 `runtime.get<T>()`、`Editor&` 或宽泛 `Context` 成为新的服务定位器。

## 1. 不可变架构决议

### A1. `modules/` 是公共 SDK 分发边界

`modules/` 中的每个公开目标必须能够被一个完全不使用 Lux ECS、Lux Scene、Lux Game 或 Lux Editor 的外部项目自然理解和采用。

准入问题不是“是否位于依赖图较低处”，而是：

> 一个独立的渲染项目、工具、模拟程序或内容处理程序，是否会主动选择这个库，并且不需要理解上层引擎语义？

因此 `modules/` 禁止出现：

```text
SceneRuntime
SceneContribution
EngineExtensions
EditorPanel
GameApplication
RuntimeLaunchManifest
Lux Engine Extension ABI
ECS EntityRegistry
org.lux.builtin.* 场景功能名称
```

### A2. `resource` 只保留 `description` 与 `asset`

`description` 描述被动的数据形状；`asset` 描述身份、文件协议、Codec、Provider、虚拟文件系统与 Pak。任何“因为 Toolchain 和 Runtime 都需要”而下沉的上层协议，都必须回到其真实语义所有者。

`resource` 不再按消费者建立一级模块：

```text
classic_mesh
deployment
entity_scene
physics3d
spatial
spatial3d_scene
terrain
tilemap
```

这些目录中的代码按语义迁往 `description`、`asset/codecs`、`ecs`、`engine` 或 `render`。

### A3. 数据格式归语义所有者，而非最低公共层

共享磁盘格式不自动属于 Resource。正确规则是：

```text
Renderer 与 Shader Toolchain 共享的协议 → render/shader
ECS Cooker 与 ECS Loader 共享的协议     → ecs/scene_format
Game Exporter 与 Game Bootstrap 共享协议 → engine/game/deployment
Model Importer 与 Material Converter 共享协议 → toolchain/model_import
```

允许生产者和消费者共同依赖一个小型格式目标；禁止为了避免横向依赖而把格式下沉到无关层。

### A4. 公共模块数量由独立产品价值决定

一个 `struct + encode/decode` 不自动获得独立安装组件。公开组件必须具备明确外部使用场景、独立依赖闭包、独立版本策略和稳定语义。

源码子目录、Object Library、私有 Static Library 与公开安装组件是四种不同概念，不能因为希望缩短 CMake 文件就增加公开组件。

### A5. Extension 是 Engine 概念

Lux Engine 动态扩展统一使用 `Extension`：

```text
ExtensionId
ExtensionDescriptor
ExtensionRegistrar
ExtensionLoader
ExtensionLease
```

它们位于 `engine/extensions`。其中 ABI 可作为独立的 `lux-engine-extension-sdk` 安装，但普通公共模块不依赖该 SDK。

以下词汇严格分离：

| 词汇 | 唯一语义 |
| --- | --- |
| Library Module | `modules/` 下的构建和分发单元，仅在构建文档中使用 |
| Extension | Lux Engine 动态扩展 |
| ScriptModule | Lua、Native 等 Script Runtime 加载单元 |
| Feature | 安装到 Scene 或 Renderer 的能力 |
| CMake Module | `.cmake` 构建脚本 |

### A6. Game 与 Editor 是产品，不是 Runtime 继承树

不创建公开 `EngineRuntime`、`GameRuntime` 或 `EditorRuntime`。目标关系为：

```text
公共库与 ECS
        ↓
Engine 领域实现
        ↓
Game 产品组合根
Editor 产品组合根
```

Editor 在 Play 时创建与导出游戏使用同一实现的 `game::Session`；Editor 常驻状态不嵌入完整 Game 产品。

### A7. ECS System 只由 Schedule 拥有

具体 `ISystem` 实例始终由某个 Scene 的 `Schedule` 唯一拥有。Engine 或 Extension 只拥有描述符、Factory 与 Feature Catalog，不拥有 System 实例。

### A8. 依赖必须显式而精确

构造完成的对象必须立即合法可用。禁止通过 `set*Service()`、`set*Hook()`、`wire()` 或可空字段完成必需依赖的两阶段装配。

组合根可以查询装配上下文；业务对象不可以。依赖从组合根解析一次，再以引用、值类型 Client、Handle 或唯一所有权传入。

### A9. Build-time 生成器依赖不得污染 Runtime 公共闭包

Meta Generator、Shader Emitter、Codegen CMake Script、libclang、MLIR 与 Shader Compiler 是构建工具。除生成产物本身确实依赖 Runtime Library 外，不能因为“生成器要解析头文件”就给 Runtime 目标增加 PUBLIC Link Dependency。

### A10. 名称必须说明真实职责

默认禁止新增 `Host`、`Manager`、`Controller`、`Service`、`Runtime` 等弱语义后缀。允许条件见第 5 节。

### A11. 关闭协议是显式状态机

不能依赖巨型 `struct` 的字段声明顺序来表达跨线程关闭。每个生命周期域必须有显式 admission close、drain、commit barrier 与资源释放顺序。

### A12. 迁移必须保持可构建

所有大型迁移采用：

```text
建立新边界
→ 增加适配层
→ 迁移调用者
→ 增加架构闸门
→ 删除旧目标与旧符号
```

不得长期保留双重概念；每个 Compatibility Alias 必须在创建时写明删除 Pull Request（拉取请求）编号或里程碑。

## 2. 目标层级与依赖方向

```text
┌──────────────────────────────────────────────────────────────┐
│ products/                                                    │
│   game / editor / launcher                                   │
└───────────────────────────────┬──────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────┐
│ engine/                                                      │
│   execution / assets / scene / extensions / spatial3d        │
│   authoring / toolchain / game / editor                      │
└───────────────────────────────┬──────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────┐
│ ecs/                                                         │
│   core / serialization / scene_format / render / physics     │
│   animation / integration                                    │
└───────────────────────────────┬──────────────────────────────┘
                                │
┌───────────────────────────────▼──────────────────────────────┐
│ modules/ — independently consumable SDK libraries            │
│   core / platform / resource / function                      │
└──────────────────────────────────────────────────────────────┘
```

允许依赖方向：

```text
modules/core
    ↑
modules/platform, modules/resource
    ↑
modules/function
    ↑
ecs
    ↑
engine
    ↑
products
```

同层领域之间只允许通过显式 Integration 叶节点组合；禁止循环依赖或把一个领域的实现细节放入更低层来“化环”。

### 2.1 层级准入测试

任何新文件合并前必须回答：

| 问题 | 若答案为“是” |
| --- | --- |
| 是否知道 Entity、Component、World 或 Schedule？ | 至少属于 `ecs/` |
| 是否知道 Scene Feature、Game Session、Extension 或 Runtime Package？ | 至少属于 `engine/` |
| 是否知道 Project、Authoring Document、Importer UI 或 Panel？ | 属于 `engine/authoring`、`engine/toolchain` 或 `engine/editor` |
| 是否包含 Vulkan/GLFW/ImGui 具体类型？ | 放入对应 Backend 或 Integration，不进入 backend-neutral 公共目标 |
| 是否只因两个上层消费者都使用而被下沉？ | 暂停合并，建立真实领域格式目标 |
| 外部非引擎项目是否能独立采用？ | 才可能属于 `modules/` |

## 3. `modules/` 的公共 SDK 合同

### 3.1 允许的公共目标

目标最终应收敛为领域名称，不暴露仓库内部层级：

```cmake
lux::events
lux::log
lux::math
lux::serialization

lux::window
lux::window_glfw
lux::dynamic_library
lux::filewatch

lux::description
lux::asset
lux::asset_pak

lux::render
lux::render_graph
lux::render_vulkan
lux::render_standard

lux::input
lux::animation
lux::navigation
lux::script
lux::script_lua
lux::script_native
lux::ui
lux::ui_imgui
```

是否公开 `lux::render_standard`、`lux::asset_pak` 等可选目标，以独立使用场景和依赖闭包为准；不能为每个内部 Feature 建立安装组件。

### 3.2 公共头文件约束

公共模块使用直接领域前缀：

```cpp
#include <lux/render/Renderer.hpp>
#include <lux/asset/Reader.hpp>
#include <lux/description/Mesh.hpp>
#include <lux/input/ActionMapper.hpp>
```

不再要求外部使用者理解：

```cpp
lux/engine/function/...
lux/engine/resource/...
lux/engine/platform/...
```

迁移期可保留 Forwarding Header，但 Forwarding Header 不得包含实现或新增 API。

## 4. Engine 与产品的目标所有权图

```text
lux::game::Game
├── engine::execution::Executor
├── engine::assets::AssetStore
├── render::Renderer
├── engine::extensions::ExtensionLoader
├── events::Events
├── input::Input
└── game::Session
    └── scene::Scene
        ├── ecs::World
        ├── ecs::Schedule
        ├── scene::Features
        └── execution::TaskGroup

lux::editor::Editor
├── 与 Game 相同种类的 Execution / Asset / Render / Extension 基础能力
├── editor::Workspace
│   ├── Project
│   ├── Content
│   ├── ContentIndex
│   └── Documents
├── editor::Workbench
│   ├── UI
│   ├── MainMenu
│   ├── PanelCatalog
│   └── Panels
├── optional<game::Session> play
└── PreviewScene[]
```

注意：

- `ExtensionLoader` 是 Engine 动态扩展加载器，不是通用服务容器。
- `Executor` 现阶段留在 Engine；只有完全剥离 Main Thread、Extension Operation 和 Engine Close 语义后，才允许重新评估是否进入公共 Modules。
- `AssetStore` 是 Engine 运行资产缓存与驻留所有者；公共 `lux::asset` 只负责文件协议与 Provider。
- `render::Renderer` 是公共渲染库的主对象；Engine 只提供 Scene/ECS 与 Renderer 的 Integration。

## 5. 统一命名规则

### 5.1 后缀规则

| 后缀 | 允许条件 | 禁止示例与目标处理 |
| --- | --- | --- |
| `System` | 仅 `ecs::ISystem` 派生，并由 `Schedule` 调度 | `UISystem → UI` |
| `Runtime` | 语言虚拟机、字节码环境或真正的执行环境 | `SceneRuntime → Scene`；`AsyncRuntime → Executor`；`ScriptRuntime` 可保留 |
| `Host` | 确实托管外部 Guest Process 或外部 Runtime | `GameHost` 删除；`RenderBackendHost → Renderer`；`MaterialPreviewHost → MaterialPreview` |
| `Manager` | 无法用 Store、Catalog、Index、Pool、Cache、Loader 表达，且拥有统一策略 | `AssetManager → AssetStore`；`ExtensionModuleManager → ExtensionLoader` |
| `Controller` | 轻状态应用用例编排，不拥有核心领域状态 | `ProjectController → Workspace`；`SceneController → Documents` |
| `Service` | 独立长期生命周期、多个消费者、拥有外部资源或工作队列 | 普通 Compiler、Codec、Repository 不使用 |
| `Module` | 只用于 Script Module、CMake Module 或文档中的 Library Module | 不创建 `engine::module::Modules` |

### 5.2 标识符与枚举

新代码采用：

```cpp
AssetId
AssetTypeId
SceneFeatureId
ExtensionId
ComponentSchemaId
```

不再新增：

```cpp
asset_id_t
EAssetType
ESceneRuntimeState
EExtensionModuleTarget
```

旧 ABI 或文件格式字段可保留原始数值布局，但 C++ 外观使用新强类型。

### 5.3 生命周期动词

| 动词 | 语义 |
| --- | --- |
| `create` | 纯对象或无外部身份资源的构造 |
| `open` | 打开文件、设备、项目、Scene 或产品，可能失败 |
| `start` | 已构造对象开始线程或活动 |
| `frame` / `update` | 推进一步 |
| `close` | 关闭 admission、排空工作并释放资源 |
| 析构 | 对已关闭或可同步关闭的资源做兜底，不承载复杂业务关闭 |

同一领域禁止并存 `initialize`、`bringUp`、`tearDown`、`shutdown`、`releaseGpu`、`stopAndDrain` 等多套生命周期词汇。

## 6. 所有权与借用规则

| 语义 | 类型 |
| --- | --- |
| 值对象 | `T` |
| 唯一所有权 | `std::unique_ptr<T>` |
| 必需且 owner 更长寿 | `T&` / `const T&` |
| 可选瞬时借用 | `T*` |
| 动态卸载对象观察 | generation handle + `ExtensionLease` |
| 异步任务输入 | owning value 或不可变快照 |
| 真正共同决定销毁 | `std::shared_ptr<T>` |
| 观察共享对象 | `std::weak_ptr<T>` |
| 远端或异步能力 | value-type Client / Ticket |
| ECS Entity/System | 强类型 ID + generation |

禁止以 `shared_ptr` 作为“现代 C++ 装饰”。例如 Panel 与 Compiler 的明确父子生命周期应使用引用，而不是共享所有权。

## 7. 格式与协议归属规则

### 7.1 三问法

每个格式类型都必须回答：

1. 这个格式描述的事实属于哪个领域？
2. 哪个领域决定其版本演进？
3. 若移除 Lux Engine 上层，外部用户是否仍会自然使用它？

示例：

| 格式 | 所有者 |
| --- | --- |
| Mesh、Texture、Skeleton 值数据 | `modules/resource/description` |
| Asset Header、Pak Index | `modules/resource/asset` |
| Render Descriptor Layout | `modules/function/render/shader` |
| ECS Entity Section Wire Image | `ecs/scene_format` |
| Game Launch Manifest | `engine/game/deployment` |
| Scene Feature 配置 | `engine/scene/package` |
| Editor Workspace Layout | `engine/editor` |

### 7.2 禁止的“语义下沉”

以下 Pull Request 必须被拒绝：

```text
“Toolchain 和 Runtime 都需要，所以移到 resource”
“ECS 和 Extension 都需要，所以移到 core”
“Renderer 和 Editor 都需要，所以移到 platform/common”
“为了消除 CMake 环，把 Scene Descriptor 放到 description”
```

正确做法是建立领域所有者提供的小型协议目标，并让两个消费者共同依赖它。

## 8. 施工标记与提交规则

| 标记 | 含义 |
| --- | --- |
| `CREATE` | 新建目标、文件或测试；不得先删除旧实现 |
| `MOVE` | 移动实现并同步 include、namespace、export、test |
| `SPLIT` | 从一个目标中分离独立职责；必须有依赖前后对比 |
| `MODIFY` | 修改现有符号、字段、签名或调用路径 |
| `ADAPT` | 暂时兼容旧调用；必须有删除里程碑 |
| `DELETE` | 全仓无引用且架构闸门生效后删除 |
| `GATE` | 必须通过的构建、测试、ABI 或静态扫描 |

每个 Pull Request 描述必须包含：

```text
1. 本次处理的架构债
2. 当前依赖闭包
3. 目标依赖闭包
4. 文件 MOVE/SPLIT/DELETE 清单
5. 兼容层及删除里程碑
6. 构建 Profile 覆盖
7. 测试与故障注入
8. 是否改变文件格式或 ABI
```

## 9. 文档执行顺序

| 顺序 | 文档 | 退出条件 |
| --- | --- | --- |
| 1 | 01 公共 SDK 边界 | `modules/` 隔离规则进入持续集成 |
| 2 | 02 Core 与 Platform | `extension_abi`、`common` 清零；GAPI 按 ADR 保留；Window 边界收敛 |
| 3 | 03 Resource | 只剩 `description` 与 `asset` 一级语义 |
| 4 | 04 Function | Render/Input/Animation/Navigation/Script/UI 公共闭包纯化 |
| 5 | 05 ECS | ECS Core 不再依赖 Engine Extension、Asset Manager 或 Engine Scene |
| 6 | 06 Engine | Execution、AssetStore、Scene、ExtensionLoader 边界稳定 |
| 7 | 07 Game 与 Editor 产品 | 共享 Session 路径；删除 GameHost |
| 8 | 08 Editor | Workspace、Workbench、Documents、Panels 完成 |
| 9 | 09 CMake 与 SDK | 公开包、namespace、安装导出完成 |
| 10 | 10 测试与验收 | 旧目标、旧 include、兼容层归零 |
| 全程 | 11 Checklist 与 12 映射表 | 每个施工项可追踪 |

## 10. 变更控制与兼容策略

### 10.1 兼容层允许范围

允许短期兼容：

```cpp
namespace lux::asset {
using asset_id_t [[deprecated("use AssetId")]] = AssetId;
}
```

允许短期 CMake Alias：

```cmake
add_library(lux::engine::resource::asset_core ALIAS lux_asset)
```

不允许：

- 新旧对象各自保存状态；
- 新接口调用旧接口、旧接口又回调新接口；
- 同一磁盘格式出现两个独立 Codec；
- Compatibility Header 中定义新行为；
- 无删除里程碑的 alias。

### 10.2 文件格式变更

涉及以下内容必须单独 Pull Request：

- Asset Header；
- Pak Index；
- ECS Entity Section；
- Scene Package Manifest；
- Extension ABI；
- Script ABI；
- Render Command Protocol。

必须提供：

```text
旧格式 golden file
新格式 golden file
向前/向后兼容矩阵
拒绝路径测试
版本迁移工具或明确的不兼容发布说明
```

## 11. 全局完成定义

项目只有在同时满足以下条件时才算完成：

- `modules/` 安装后可以在独立外部样例中使用，不需要 `ecs/` 或 `engine/`。
- `modules/core/extension_abi`、`modules/platform/common`、`modules/resource/deployment` 等已裁决错误目录消失；`modules/platform/gapi` 按保留 ADR 继续存在。
- `modules/resource` 只有 `description` 与 `asset` 一级语义。
- `AssetManager`、`EntityRegistry`、Scene Package、Extension ABI 已回到正确层。
- ECS Core 不依赖 Engine Extension、Engine Asset Store 或 Game Scene Manifest。
- Game 与 Editor 不依赖公开 `EngineRuntime`。
- Editor Panel 不保存通用 Runtime/Editor 引用，不使用必需依赖 setter。
- Extension、ScriptModule、Library Module 的术语无交叉。
- 公共 CMake Target 与 Include Prefix 使用领域名称。
- PLAYER、EDITOR、TOOLCHAIN、DEVELOPER Profile 均通过双构建与测试。
- 架构扫描器能够阻止旧依赖和旧命名重新进入。


---

# `modules/` 公共 SDK 边界与分发体系重构

> 把 modules 从“依赖图底层”恢复为可独立采用的库产品，并建立可自动验证的公共边界

**执行文档 01 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | 公共 SDK 负责人、CMake 维护者、Render/Input/Resource 等模块负责人、发布工程师 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 公共 SDK 可以拥有与 Engine 语义无关的完整资产机制。现有 `asset_id_t`、`LuxAsset`、`AssetManager`、Codec Catalog、Provider、VFS 与 Pak 保留在 `modules/resource/asset`；旧版提出的 `AssetId`/`AssetTypeId` 改名和 AssetStore 上移目标由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。

> **2026-08-21 裁决更新：** Provider/VFS 是 opaque bytes 存储面，Pak v2 的 reader/writer/inspector 都是公共 Asset SDK；Asset 源码按 texture/material/mesh/model/animation/shader/script/storage 领域内聚。冻结的 Engine 内置资产 ID 不属于 Modules。详见 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。

> **2026-08-21 Profile 修订：** 不新增 `MODULES_SDK` Profile。公共 SDK 独立性通过现有四个 Profile 的安装结果和 installed consumers 验证；本文后续 `MODULES_SDK` 示例仅为历史提案，不再施工。Modules 聚合目录仍必须改为显式子目录列表。

> **2026-08-21 Extension ABI 实施状态：** `modules/core/extension_abi` 已删除；ABI v4 只由独立 `lux-engine-extensions` package 的 `extension_api` component 安装，Core/Modules package 不再导出 Extension API。


## 1. 当前问题与施工目标

当前 README 将 `modules/platform → core → resource → function` 描述为 ECS 与 Engine 之下的复用层，这一方向正确；但实际目录已经混入 Engine Extension ABI、Game Deployment Manifest、ECS Entity Scene、World Partition 配置、Runtime Reflection Entity Registry 等上层语义。

本工作流的目标不是把这些目录重新命名，而是建立以下可执行合同：

```text
只安装 modules SDK
→ 能配置和编译独立 Render/Input/Asset/UI 示例
→ 不配置 ecs/
→ 不配置 engine/
→ 不出现 SceneRuntime、EngineExtensions、EditorPanel、GameApplication
```

### 1.1 当前目录裁决

| 当前目录 | 动作 | 目标公开面 | 说明 |
| --- | --- | --- | --- |
| `modules/core/events` | 保留 | `lux::events` | 清理对 Host/AsyncRuntime 的说明；API 只表达延迟事实分发 |
| `modules/core/log` | 保留 | `lux::log` | Sink 接口保持可注入；Engine 异步 Sink 留在 Engine |
| `modules/core/math` | 保留并扩充 | `lux::math` | 吸收 `resource/spatial` 的纯值类型 |
| `modules/core/serialization` | 拆分 | `lux::serialization` | 只保留 Archive/NameTable/Byte primitives；反射 Component Archive 上移 `ecs/serialization` |
| `modules/core/meta` | 拆除聚合边界 | `lux-cxx::reflection_runtime` + `ecs` + `engine/reflection` | EntityRegistry 与 Extension Sidecar 语义移出 |
| `modules/core/extension_abi`（已删除） | 已搬迁 | `engine/extensions/api` | DONE；作为单独 Engine Extension SDK，而非公共 Core |
| `modules/platform/common` | 删除 | 分别迁移 | AtomicWait/Format/Size2D/ImageEnums 各归其领域 |
| `modules/platform/dynamic_library` | 保留 | `lux::dynamic_library` | 独立 RAII 动态库加载 |
| `modules/platform/filewatch` | 保留 | `lux::filewatch` | Editor 只是当前消费者，不改变通用性 |
| `modules/platform/window` | 拆分 | `lux::window`、`lux::window_glfw` | 核心抽象、平台 Backend、Vulkan Surface Integration 分离 |
| `modules/platform/gapi` | 保留 | 当前 owner | 公共低层 Vulkan Wrapper SDK；Render 可消费但不取得所有权 |
| `modules/resource/description` | 保留并纯化 | `lux::description` | 只保留被动资源值类型 |
| `modules/resource/asset` | 保留并重构 | `lux::asset` | 通用 Asset/SerDeser/Catalog/Manager/Provider/VFS 与 Pak 读写；不含 Engine 默认内容 |
| `modules/resource/*` 其他目录 | 消除一级组件 | 按语义迁移 | 见文档 03 |
| `modules/function/render` | 保留并重构 | `lux::render*` | 公共渲染库主边界 |
| `modules/function/input` | 保留并解耦 | `lux::input` | 不 PUBLIC 依赖 GLFW/Window Backend |
| `modules/function/animation` | 保留并解耦 | `lux::animation` | 依赖 Description，不依赖 AssetStore |
| `modules/function/navigation` | 保留 | `lux::navigation` | 依赖 Math/Spatial 值 |
| `modules/function/script` | 保留 | `lux::script*` | `ScriptModule` 仅指脚本模块 |
| `modules/function/ui` | 拆分 | `lux::ui*` | 基础 UI、ImGui、GLFW、Vulkan、Editor Viewport 分离 |

## 2. 公共 SDK 准入标准

一个目标只有同时满足以下条件，才允许安装为 `modules/` 公共组件：

1. 名称是外部用户能直接理解的领域名称；
2. 公共头文件不包含 `lux/engine/runtime`、`lux/engine/editor`、`lux/engine/hosts`；
3. PUBLIC Link 闭包不包含 `ecs`、`engine/runtime`、`engine/editor`、`engine/toolchain`；
4. 不依赖 Lux Engine Extension ABI；
5. 不依赖具体产品 Manifest；
6. 具有独立外部使用场景；
7. 具有独立测试；
8. 安装后的 Config Package 能单独 `find_package`；
9. 公共 ABI 不暴露私有 Backend 类型；
10. Build-time Generator 不通过 PUBLIC Link 泄漏到消费者。

### 2.1 不足以成为公共组件的理由

以下理由单独存在时均不成立：

```text
“这个目录有自己的 CMakeLists.txt”
“这个 Blob 有独立 Codec”
“两个上层模块都使用”
“拆成组件后增量编译更快”
“这样 install_components 列表更清楚”
```

增量编译问题应使用 Object Library、Unity 分组、私有 Static Library 或源文件分区解决。

## 3. 目标安装与命名空间

### 3.1 目标包

第一阶段不要求立即拆成多个仓库，但安装包应按外部领域组织：

| 包 | 主要目标 | 不得携带 |
| --- | --- | --- |
| `lux-core` | `lux::events`、`lux::log`、`lux::math`、`lux::serialization` | ECS、Extension、AssetManager |
| `lux-platform` | `lux::window`、`lux::window_glfw`、`lux::dynamic_library`、`lux::filewatch` | Renderer、Game、Editor |
| `lux-resource` | `lux::description`、`lux::asset`、`lux::asset_pak` | Scene、Deployment、World Partition |
| `lux-render` | `lux::render`、`lux::render_graph`、`lux::render_vulkan`、`lux::render_standard` | ECS、Editor、Game Session |
| `lux-input` | `lux::input` | GLFW 类型、Window 对象 |
| `lux-animation` | `lux::animation` | AssetStore、ECS Animator |
| `lux-navigation` | `lux::navigation`、`lux::navigation_detour3d` | ECS NavigationSystem |
| `lux-script` | `lux::script`、`lux::script_lua`、`lux::script_native` | Engine Extension |
| `lux-ui` | `lux::ui`、`lux::ui_imgui`、可选 Backend Integration | SceneViewport、Editor Panel |

### 3.2 目标 Include Prefix

```cpp
#include <lux/math/Position.hpp>
#include <lux/asset/Reader.hpp>
#include <lux/render/Renderer.hpp>
#include <lux/input/ActionMapper.hpp>
#include <lux/script/Runtime.hpp>
```

迁移期 Forwarding Header：

```cpp
// old: lux/engine/resource/asset/AssetId.hpp
#pragma once
#include <lux/asset/AssetId.hpp>
```

Forwarding Header 必须：

- 只包含新头；
- 带弃用注释；
- 在 `SDK-COMPAT-REMOVE` 里程碑删除；
- 不被新代码 include。

## 4. 精确 CMake 修改

### 4.1 根 CMake 增加 SDK-only Profile

CREATE `cmake/Profiles/ModulesSdk.cmake`：

```cmake
set(LUX_BUILD_MODULES_SDK ON CACHE BOOL "" FORCE)
set(LUX_BUILD_ECS         OFF CACHE BOOL "" FORCE)
set(LUX_BUILD_ENGINE      OFF CACHE BOOL "" FORCE)
set(LUX_BUILD_PRODUCTS    OFF CACHE BOOL "" FORCE)
```

MODIFY 根 `CMakeLists.txt`，将当前目录添加逻辑改为显式门控：

```cmake
add_subdirectory(modules)

if(LUX_BUILD_ECS)
    add_subdirectory(ecs)
endif()

if(LUX_BUILD_ENGINE)
    add_subdirectory(engine)
endif()

if(LUX_BUILD_PRODUCTS)
    add_subdirectory(products)
endif()
```

若当前 Profile 系统不适合增加布尔门控，则新增 `MODULES_SDK` Profile；关键验收是配置过程中完全不访问 `ecs/` 与 `engine/`。

### 4.2 删除目录枚举式自动发现

当前多个层使用 `subdirectory_list(dir_list)` 自动添加所有子目录。公共 SDK 边界必须改为显式列表，否则新建一个实验目录就会自动进入安装闭包。

MODIFY：

```text
modules/core/CMakeLists.txt
modules/platform/CMakeLists.txt
modules/resource/CMakeLists.txt
modules/function/CMakeLists.txt
```

目标写法：

```cmake
add_subdirectory(events)
add_subdirectory(log)
add_subdirectory(math)
add_subdirectory(serialization)
```

禁止：

```cmake
subdirectory_list(dir_list)
foreach(subdir ${dir_list})
    add_subdirectory(${subdir})
endforeach()
```

### 4.3 分类属性与公开属性分离

`LAYER / PRODUCT / ROLE` 是仓库内部持续集成元数据，不应进入安装后的公共 Target 名称。保留：

```cmake
lux_classify_target(
    TARGET  render
    LAYER   FUNCTION
    PRODUCT SDK
    ROLE    DOMAIN
)
```

但公开 Alias 使用：

```cmake
add_library(lux::render ALIAS lux_render)
```

而不是：

```cmake
lux::engine::function::render_vulkan
```

### 4.4 安装包兼容策略

迁移期同时导出：

```cmake
lux::render
lux::engine::function::render_vulkan  # deprecated alias
```

旧 Alias 不能成为真实目标，必须是新目标的 ALIAS 或 imported forwarding target。

## 5. 目录级施工清单

### 5.1 CREATE

```text
cmake/Profiles/ModulesSdk.cmake
cmake/Architecture/ValidateModulesSdk.cmake
tests/sdk/modules_minimal/
tests/sdk/render_minimal/
tests/sdk/asset_minimal/
tests/sdk/input_minimal/
tests/sdk/script_minimal/
```

### 5.2 MODIFY

```text
CMakeLists.txt
readme.md
modules/CMakeLists.txt
modules/core/CMakeLists.txt
modules/platform/CMakeLists.txt
modules/resource/CMakeLists.txt
modules/function/CMakeLists.txt
cmake/TargetArchitecture.cmake
cmake/WriteRuntimeDependencyInventory.cmake
```

`readme.md` 的依赖图改为：

```text
modules/core
  ├─ modules/platform
  ├─ modules/resource
  └─ modules/function
           ↓
          ecs
           ↓
         engine
           ↓
        products
```

不再把 `Resource` 视为所有共享协议的默认归宿。

### 5.3 MOVE / DELETE

具体文件迁移由文档 02–06 执行；本文件先建立闸门，防止迁移期间继续新增错位代码。

## 6. SDK 架构扫描器

CREATE `cmake/Architecture/ValidateModulesSdk.cmake` 或等价 Python 脚本，检查所有 `modules/**/include`：

```text
禁止 include 前缀：
  lux/engine/runtime/
  lux/engine/editor/
  lux/engine/hosts/
  lux/engine/authoring/
  lux/engine/toolchain/
  lux/engine/ecs/

禁止公开符号词：
  SceneRuntime
  GameApplication
  LuxEditor
  EngineExtensions
  RuntimeContributionRegistrar
  EditorContributionRegistrar
```

例外不得通过全局 Allowlist；必须限定到具体过渡文件，并写明删除版本。

### 6.1 PUBLIC Link 闭包检查

对每个安装目标执行：

```cmake
get_target_property(_links target INTERFACE_LINK_LIBRARIES)
```

递归解析后拒绝：

```text
LAYER=ECS
LAYER=RUNTIME
LAYER=EDITOR
LAYER=TOOLCHAIN
LAYER=HOST
PRODUCT=PLAYER
PRODUCT=EDITOR
```

### 6.2 构建工具泄漏检查

检查安装后的 `*Config.cmake` 不包含：

```text
libclang
MLIR
LLVM
shaderc
meta_generator
lux_shader_emitter
engine_add_meta.cmake 的构建树绝对路径
```

## 7. 外部消费样例

### 7.1 Render-only

```cmake
cmake_minimum_required(VERSION 3.25)
project(render_sample LANGUAGES CXX)

find_package(lux-render CONFIG REQUIRED)
find_package(lux-platform CONFIG REQUIRED COMPONENTS window_glfw)

add_executable(render_sample main.cpp)
target_link_libraries(render_sample
    PRIVATE
        lux::render
        lux::render_vulkan
        lux::window_glfw)
```

样例不得链接：

```text
lux-engine-ecs
lux-engine-runtime
lux-engine-editor
lux-engine-extension-sdk
```

### 7.2 Asset-only

```cmake
find_package(lux-resource CONFIG REQUIRED COMPONENTS asset description)

add_executable(asset_inspect main.cpp)
target_link_libraries(asset_inspect PRIVATE lux::asset lux::description)
```

### 7.3 SDK-only 构建闸门

持续集成新增：

```bash
cmake -S . -B build/modules-sdk   -DLUX_BUILD_PROFILE=MODULES_SDK

cmake --build build/modules-sdk --target all
ctest --test-dir build/modules-sdk --output-on-failure
```

随后使用安装结果配置上述外部样例，而不是在源树内直接链接目标。

## 8. 公共 API 版本策略

每个公共模块建立：

```text
API_VERSION
ABI_VERSION
FILE_FORMAT_VERSION（若有）
```

三者不得混为一个版本。

例如：

```text
lux::asset API v2
Asset file header v3
Pak index v2
```

Extension ABI 不属于公共 Asset/Core 版本；它由 `lux-engine-extension-sdk` 独立管理。

## 9. Pull Request 序列

| PR | 内容 | 必须保持 |
| --- | --- | --- |
| SDK-01 | 增加 `MODULES_SDK` Profile 与隔离测试 | 不移动任何实现 |
| SDK-02 | 显式列举 modules 子目录，停止自动发现 | 全 Profile 配置一致 |
| SDK-03 | 引入新公共 Alias 与 Include Prefix | 旧 Alias 仍工作 |
| SDK-04 | 接入 Include/Link 闭包扫描 | 允许受控兼容例外 |
| SDK-05+ | 按文档 02–04 迁移错位模块 | 每个 PR 可独立回滚 |
| SDK-FINAL | 删除旧包名、旧 Include、例外清单 | 外部样例全部使用新 API |

## 10. 验收闸门

- [ ] `MODULES_SDK` 配置不执行 `add_subdirectory(ecs)`。
- [ ] `MODULES_SDK` 配置不执行 `add_subdirectory(engine)`。
- [ ] 安装结果中不存在 `SceneRuntime.hpp`、`EngineExtensions.hpp`、`GameApplication.hpp`。
- [ ] 公共目标的递归 Link Closure 不包含 ECS/Engine/Editor。
- [ ] Render-only 外部样例可以创建窗口、初始化 Vulkan Renderer 并清理。
- [ ] Asset-only 外部样例可以读取 Header、选择 Codec、读取 Pak。
- [ ] Input-only 外部样例不需要 GLFW。
- [ ] Script-only 外部样例不需要 Engine Extension ABI。
- [ ] 新公共头不使用 `lux/engine/function` 或 `lux/engine/resource` 前缀。
- [ ] 所有兼容 Alias 都有删除里程碑。


---

# Core 与 Platform 基础库清理

> 移除 Engine、ECS 与 Vulkan 语义对基础层的污染，并把 common/meta/extension_abi 拆回真实所有者

**执行文档 02 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Core、Platform、Reflection、Serialization、ECS 与 Render 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 关联裁决：** 本文 Core/Platform 目标不变。资产身份、Scene Asset 与场景 Payload 的现行边界见 `ADR-20260820_SceneAsset与Resource边界.md`；不得为该迁移把 Engine Scene 语义下沉到 Core。

> **2026-08-21 裁决更新：** Core Meta 删除 EnTT、Registry、`LuxObject` 与 `EntityObject`；Registry allocator/handle/publication 合同原样归 `ecs/core`。反射类型由标注 record identity 决定，不再依赖 OO 根类。详见 `ADR-20260821_CoreMeta纯化与ECSRegistry归位.md`。

> **2026-08-21 实施状态：** 上述 Meta/Registry 裁决已完成。Core Meta/Serialization installed consumer 不导入 EnTT；ECS-owned adapter 承担 EnTT component 操作，旧 Meta Registry 与 OO 根类已归零。

> **2026-08-21 Serialization 实施状态：** `d1ead288` 已将 reflected tagged archive 整体迁入 ECS `component_archive`。Core Serialization 只导出 Archive/NameTable，安装闭包不含 Meta 或 Eigen；旧 Core 头与 namespace 已删除。

> **2026-08-21 Extension ABI 实施状态：** `c56efbc4` 已把 v4 实体 owner 迁入 `engine/extensions/api`，删除 Core 目录/target/component/include/export 与通用 `ContributionId`；ABI-facing 名称、布局与导出 symbol 不变。


## 1. 施工范围

本文件处理以下目录：

```text
modules/core/events
modules/core/log
modules/core/math
modules/core/meta
modules/core/serialization
modules/core/extension_abi（已删除，保留为历史施工范围）

modules/platform/common
modules/platform/dynamic_library
modules/platform/filewatch
modules/platform/gapi
modules/platform/window
```

目标不是把所有东西都压入 `core`。Core 只容纳真正通用、与 Engine/ECS 无关的基础能力；Platform 只容纳操作系统与平台边界。

## 2. 文件迁移总表

| 当前文件/目录 | 目标 | 动作 |
| --- | --- | --- |
| `modules/core/extension_abi/include/.../StableId.hpp`（已删除） | `lux-cxx::StableNameId` remains generic; domain IDs move to owners | DONE — SPLIT/DELETE |
| `modules/core/extension_abi/include/.../ModuleAbi.hpp`（已删除） | `engine/extensions/api/include/lux/engine/extensions/ExtensionAbi.hpp` | DONE — MOVE/DELETE |
| `modules/core/meta/include/lux/engine/meta/LuxObject.hpp` | `ecs/core/include/lux/ecs/EntityRegistry.hpp` and optional `EntityObject.hpp` | SPLIT |
| `modules/core/meta/include/lux/engine/meta/Meta*.hpp` | `lux-cxx::reflection_runtime` or temporary `engine/reflection` | SPLIT |
| `modules/core/meta/cmake/engine_add_meta.cmake` | `cmake/Codegen/Reflection.cmake` | MOVE |
| `modules/core/serialization/src/TaggedPropertyArchive.cpp`（已删除） | `ecs/serialization` Component Archive；Core 保留 Archive/NameTable | DONE — MOVE/DELETE |
| `modules/resource/spatial/include/.../Spatial.hpp` | `modules/core/math/include/lux/math/Position.hpp` and `Grid.hpp` | MOVE |
| `modules/platform/common/include/.../AtomicWait.hpp` | `lux-cxx::concurrent` or `modules/core/concurrency` | MOVE |
| `modules/platform/common/FormatCompat.h.in` | `lux-cxx::format` or `modules/core/format` | MOVE |
| `modules/platform/common/include/.../Size2D.hpp` | `modules/core/math/include/lux/math/Extent.hpp` | MOVE |
| `modules/platform/common/include/.../ImageEnums.hpp` | `modules/resource/description/include/lux/description/Image.hpp` | MOVE |
| `modules/platform/gapi` | 当前 owner | KEEP — ADR-20260821 GAPI 保留裁决 |
| `modules/platform/window/src/GlfwRuntime.cpp` | `modules/platform/window/glfw/src/GlfwLibrary.cpp` | MOVE/RENAME |
| `modules/platform/window/src/TrayIconWin32.cpp` | `modules/platform/tray/win32` or product layer | MOVE |

## 3. `extension_abi` 迁出 Core

### 3.1 当前错误

`ModuleAbi.hpp` 当前直接定义：

```text
Lux Engine Extension ABI v4
Runtime / Editor Extension Target
RuntimeContributionRegistrar
EditorContributionRegistrar
动态库导出符号
```

这不是 Core ABI，而是 Lux Engine Extension SDK。

### 3.2 目标目录

CREATE（2026-08-21 ADR 修订后）：

```text
engine/extensions/api/
├── CMakeLists.txt
├── include/lux/engine/extensions/
│   ├── ExtensionId.hpp
│   ├── ExtensionVersion.hpp
│   ├── ExtensionDescriptor.hpp
│   ├── ExtensionAbi.hpp
│   ├── ExtensionResult.hpp
│   └── ExtensionRegistrarFwd.hpp
└── test/
    └── extension_abi_layout_test.cpp
```

MOVE：

```text
ModuleAbi.hpp → ExtensionAbi.hpp
ExtensionIdTag/ExtensionId → ExtensionId.hpp
ExtensionDependencyView → ExtensionDependencyView
ExtensionModuleDescriptorV4 → ExtensionModuleDescriptorV4
```

ABI v4 冻结：

```text
kExtensionAbiV4
EExtensionModuleTarget
ExtensionRegistrationResult
EExtensionRegistrationError
GetExtensionModuleV4Fn
RuntimeContributionRegistrar
EditorContributionRegistrar
```

上述 C++ 名称、对象布局与 ABI 导出字符串共同构成现行 v4 plugin surface。本轮只迁移
owner/include/target，不在 v4 内改名；若需要新命名，必须通过独立 ABI v5 裁决。详见
`ADR-20260821_ExtensionAbiV4Owner与Core清零.md`。

### 3.3 Stable ID 解耦

DELETE `StableId.hpp` 中对 `ExtensionId` 与 `ContributionId` 的混合定义。

通用基础只保留已有：

```cpp
lux::cxx::StableNameId<Tag>
lux::cxx::StableNameIdView<Tag>
```

各领域自己定义：

```cpp
namespace lux::ecs {
struct ComponentSchemaIdTag;
using ComponentSchemaId = lux::cxx::StableNameId<ComponentSchemaIdTag>;
}

namespace lux::render {
struct FeatureIdTag;
using FeatureId = lux::cxx::StableNameId<FeatureIdTag>;
}

namespace lux::engine::scene {
struct FeatureIdTag;
using FeatureId = lux::cxx::StableNameId<FeatureIdTag>;
}

namespace lux::extensions {
struct ExtensionIdTag;
using ExtensionId = lux::cxx::StableNameId<ExtensionIdTag>;
}
```

禁止重新引入跨领域 `ContributionId`。

## 4. `meta` 拆分

### 4.1 当前职责

当前 `modules/core/meta` 同时包含：

```text
通用 RefType/RefClass/RefField
ReflectionRegistry
代码生成 CMake
EntityRegistry / EntityHandle / EntityObject
动态库加载后的 pending sidecar drain
RegistryMemoryResource
```

这是至少三个领域。

### 4.2 拆分目标

#### A. 通用 Reflection Runtime

优先合入 `lux-cxx::reflection_runtime`：

```text
RefType
RefClass
RefField
RefMethod
AnnotationView
ReflectionDraft
ReflectionRegistry 的纯注册/查询能力
```

若短期无法合入 sibling repository，建立临时：

```text
modules/core/reflection_runtime/
```

但其验收条件是：

- 不 include EnTT；
- 不定义 Entity；
- 不知道 Extension；
- 不知道 Scene；
- 不调用 DynamicLibrary；
- 不包含 Engine 产品符号。

#### B. ECS Registry

MOVE 到 `ecs/core`：

```text
EntityRegistryBase
EntityRegistry
EntityHandle
ConstEntityHandle
RegistryMemoryResource
EntityObject（若仍需要）
```

目标接口：

```cpp
namespace lux::ecs
{
    using Entity = entt::entity;
    inline constexpr Entity nullEntity = entt::null;

    class Registry final : public RegistryBase
    {
    public:
        Registry();
        explicit Registry(RegistryMemoryUpstream& upstream);

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
    };
}
```

`LuxObject` 若只为统一虚析构存在，应删除。若生成系统确实需要标记，使用无状态 Concept/trait：

```cpp
template<class T>
concept ReflectedObject = requires { typename T::lux_reflected_tag; };
```

#### C. Engine Reflection Publication

动态 Extension 加载后的 Draft Commit、Sidecar Lease 与 unload protection 放入：

```text
engine/extensions/reflection/
```

不能让基础 Reflection Registry 知道 `ExtensionLoader`。

### 4.3 生成器 CMake

MOVE：

```text
modules/core/meta/cmake/engine_add_meta.cmake
→ cmake/Codegen/Reflection.cmake
```

Build Tool 依赖使用：

```cmake
add_dependencies(target generated_meta)
target_sources(target PRIVATE ${generated_sources})
```

禁止通过：

```cmake
target_link_libraries(target PUBLIC meta_generator)
```

把生成器带入安装闭包。

## 5. Serialization 拆分

### 5.1 保留在公共 Core

```text
ArchiveReader
ArchiveWriter
NameTable
ByteCursor
BoundsCheckedReader
基础 Tagged Record Wire Format
UUID 编解码
```

目标公开依赖不应包含 EnTT 或 Engine Reflection。

### 5.2 上移的 Adapter

CREATE：

```text
ecs/serialization/
    ComponentArchive.hpp/.cpp

engine/serialization/
    ExtensionSchemaMigration.hpp/.cpp
```

`TaggedPropertyArchive` 依赖 `RefClass/RefField`，整体归入 ECS Component Archive；Core 不再保留 tagged wire facade：

```cpp
// ecs/serialization
ComponentArchiveResult<void>
TaggedPropertyWriter::writeObject(const RefClass&, const void*);
```

### 5.3 CMake 修改

MODIFY `modules/core/serialization/CMakeLists.txt`：

```cmake
target_link_libraries(serialization
    PUBLIC
        lux::cxx::binary
        stduuid
)
```

移除 PUBLIC：

```text
lux::engine::core::meta
Eigen3::Eigen（若仅个别 Adapter 使用）
```

Eigen leaf Codec 是 ECS Component Archive 的 PRIVATE 实现依赖。

`RegistryArchive` 提案被 `EntitySectionImage -> EntityBatchStager -> Registry` 边界取代；
Core/ECS 均不建立 `entt::registry` 文件镜像。

## 6. Math 吸收 Spatial 值

### 6.1 文件拆分

MOVE：

```text
modules/resource/spatial/include/.../Spatial.hpp
→ modules/core/math/include/lux/math/Position.hpp
→ modules/core/math/include/lux/math/Grid.hpp
→ modules/core/math/include/lux/math/RelativePosition.hpp
```

目标：

```cpp
namespace lux::math
{
    struct Position2d final { double x{}; double y{}; };
    struct Position3d final { double x{}; double y{}; double z{}; };

    struct GridCoord2i64 final { std::int64_t x{}; std::int64_t y{}; };
    struct GridCoord3i64 final { std::int64_t x{}; std::int64_t y{}; std::int64_t z{}; };

    [[nodiscard]] std::optional<std::array<float, 3>>
    relative(const Position3d&, const Position3d&, float maximumExtent) noexcept;
}
```

不要在值类型头中包含：

```text
MetaAnnotations.hpp
MetaDef.hpp
ReflectionRegistry
```

反射声明由 ECS sidecar 或 Codegen 输入清单维护。

### 6.2 迁移调用点

全仓替换：

```text
lux::spatial::Position2D → lux::math::Position2d
lux::spatial::Position3D → lux::math::Position3d
lux::spatial::GridCoord2i64 → lux::math::GridCoord2i64
lux::spatial::GridCoord3i64 → lux::math::GridCoord3i64
```

旧 namespace 可以保留一个版本的 alias header，但新代码禁止使用。

## 7. 删除 `platform/common`

### 7.1 逐文件归属

| 文件 | 目标 |
| --- | --- |
| `AtomicWait.hpp` | 优先贡献到 `lux-cxx::concurrent`；临时可放 `modules/core/concurrency` |
| `FormatCompat.h.in` | 优先贡献到 `lux-cxx::format`；临时 `modules/core/format` |
| `Size2D.hpp` | `lux/math/Extent.hpp` |
| `ImageEnums.hpp` | `lux/description/Image.hpp` |

DELETE：

```text
modules/platform/common/CMakeLists.txt
lux::engine::platform::common
lux-engine-platform common component
```

所有依赖者必须改为精确目标，不能创建新的 `foundation_common`。

## 8. `gapi` 保留为 Platform 公共 SDK

### 8.1 当前事实

`modules/platform/gapi` 查找 Vulkan，并导出 Buffer、Image、Descriptor、Pipeline、Swapchain、Surface 等低层 Wrapper。按 ADR-20260821，它是外部项目也可直接使用的公共 Platform 图形 API，而不是必须被 Render 吸收的临时实现目录。

### 8.2 保留边界

- `modules/platform/gapi`、`lux::engine::platform::gapi`、安装 component 与公共头保持不变。
- Render Vulkan 可以依赖 GAPI，但不取得其类型和生命周期合同的所有权。
- GAPI wrapper 与 Render 内部 handle 服务不同公共受众，不在本重构中强制合并。
- 本阶段不修改 GAPI production 代码；后续正常维护不属于目录清零任务。

## 9. Window 拆分

### 9.1 目标目标

```text
lux::window          平台中立 Window、事件、NativeHandle
lux::window_glfw     GLFW Backend
lux::window_android  Android Backend
lux::render_vulkan_window  Window ↔ Vulkan Surface Integration
```

### 9.2 公共 Window API

```cpp
namespace lux::platform
{
    struct WindowExtent { std::uint32_t width{}; std::uint32_t height{}; };

    struct NativeWindowHandle
    {
        void* value{};
        NativeWindowKind kind{};
    };

    class Window
    {
    public:
        virtual ~Window() = default;
        virtual void pollEvents() = 0;
        virtual bool closeRequested() const noexcept = 0;
        virtual WindowExtent extent() const noexcept = 0;
        virtual NativeWindowHandle nativeHandle() const noexcept = 0;
    };
}
```

Window Core 不 include Vulkan。Vulkan Surface 创建位于：

```cpp
render::vulkan::createSurface(
    VulkanInstance&,
    platform::NativeWindowHandle);
```

### 9.3 GLFW 依赖

当前 `window` 对外 PUBLIC 链接 GLFW，理由是测试和工具直接调用 `glfwGetKey`。目标是所有输入通过 `InputSnapshot`，因此：

```cmake
target_link_libraries(window_glfw PRIVATE glfw)
```

外部消费者不再自动获得 GLFW 头和 ABI。

### 9.4 Tray Icon

`TrayIconWin32.cpp` 不属于 Window Core。若仅 Launcher 使用，迁入：

```text
products/launcher/platform/win32
```

若确认是通用 SDK 能力，建立独立 `lux::tray`，不得留在 Window。

## 10. 保留模块的清理

### 10.1 Events

RENAME 可分阶段进行：

```text
DomainEvents → Events
EventPump → Dispatcher 或内部 Queue
```

公开文档不再说“Host-owned Pump”；改为“owner-thread dispatch point”。

### 10.2 Log

Engine 的异步转发器留在 `engine/logging`。公共 Log 只提供：

```cpp
Logger
Sink
LogRecord
Category
Level
```

### 10.3 Dynamic Library 与 Filewatch

保持现有 RAII 和后端隐藏；仅迁移 include prefix 与 target alias。

## 11. Pull Request 序列

| PR | 施工内容 | 退出闸门 |
| --- | --- | --- |
| CORE-01 | 新建新路径与 compatibility headers | 无行为变化 |
| CORE-02 | Spatial 值迁入 Math | ECS/Navigation/Render 全部通过 |
| CORE-03 | Serialization 基础与反射 Adapter 分离 | 公共 Serialization 无 EnTT |
| CORE-04 | EntityRegistry 迁入 ECS | Meta 公共目标不再链接 EnTT |
| CORE-05 | Extension ABI 迁入 Engine SDK | Modules SDK 无 Extension 类型 |
| PLATFORM-01 | 解散 common | 所有依赖改为精确目标 |
| PLATFORM-02 | GAPI 保留裁决 | 旧迁移/删除目标标记 SUPERSEDED，公共 component 保持可链接 |
| PLATFORM-03 | Window Core/Backend/Surface 拆分 | Input/Window/Render 样例独立 |
| CORE-FINAL | 删除旧 target/include/namespace | 架构扫描无例外 |

## 12. 验收闸门

- [ ] `modules/core` 不定义 Entity、Scene、Extension。
- [x] `modules/core` 公共目标不链接 EnTT。
- [ ] `lux::serialization` 不链接 Engine Reflection Registry。
- [ ] `modules/platform` 不包含 Vulkan Object Wrapper。
- [ ] `lux::window` 公共头不 include `<vulkan/vulkan.h>`。
- [ ] `lux::window` 不 PUBLIC 链接 GLFW。
- [ ] `platform/common` target 已删除。
- [ ] `resource/spatial` target 已删除。
- [ ] `extension_abi` 不出现在 Modules SDK 安装清单。
- [ ] 所有 Build-time 生成器依赖不进入安装 Config。


---

# Resource：Description 与 Asset 重构

> 恢复 Resource 的通用 SDK 语义，保留既有资产机制，并把 Engine/ECS 场景语义移出 Resource

**执行文档 03 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Resource、Asset、Toolchain、ECS Scene Format、Render Content 与 Engine Asset 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 本文原有“文件协议与 Engine AssetStore 分离”、新建 `AssetId/AssetTypeId`、把 Terrain/Tilemap/Physics3D 长期放在 Resource 的目标已由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。现有 AssetManager/SerDeser/Catalog/VFS/Pak 是公共 SDK 的正式组成；Scene 是 Engine-owned Asset，三类场景 Payload 最终归对应 ECS 领域。

> **2026-08-21 裁决更新：** Asset 公共面按资产领域族组织，存储面收敛到 `asset/storage`；Pak v2 读、写和检查均属 Modules Asset SDK。`BuiltinAssetIds.hpp`、M_Missing 与演示色板迁入 `engine/content`。详见 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。

> **2026-08-21 加载链裁决：** `AssetSerDeser` 是唯一具体 Codec 多态接口，Catalog 产出完整未注册 `LuxAsset`，AssetManager 只负责安装与账本；删除 Injector/decode 回调。AssetRef 不自动 IO。详见 `ADR-20260821_Asset运行期需求与SerDeser边界.md`。

> **2026-08-21 实施状态：** 加载链裁决已完成；同步 ensure 与异步 LoadService 共用 Catalog decode/Manager install，Asset installed consumer 不导入 Core Meta，全部既有 wire Golden 保持不变。


## 1. 最终边界

`modules/resource` 最终只有两个一级目录：

```text
modules/resource/
├── description/
└── asset/
```

两者职责：

| 目录 | 负责 | 不负责 |
| --- | --- | --- |
| `description` | 被动值类型、不可变数据形状、与设备/场景无关的资源描述 | Asset Cache、Scene、Extension、产品 Manifest、Editor、运行线程 |
| `asset` | `asset_id_t`、`LuxAsset`、`AssetManager`、`AssetRef`、Header、SerDeser、Catalog、Provider、VFS、Loose/Pak | Engine Scene 语义、GPU 上传、ECS Scene Payload、Editor ContentIndex |

## 2. 当前文件逐项归属

| 当前内容 | 目标 | 动作 |
| --- | --- | --- |
| `modules/resource/description/AnimationClip.hpp` | `description/animation/AnimationClip.hpp` | 保留 |
| `.../Skeleton.hpp` | `description/animation/Skeleton.hpp` | 保留 |
| `.../Mesh.hpp`、`Vertex.hpp`、`Model.hpp` | `description/mesh/*` | 保留/整理 |
| `.../Texture.hpp`、`TextureAtlas.hpp` | `description/image/*` | 保留/整理 |
| `.../MaterialEnums.hpp` | `description/material/Material.hpp` | 保留纯值 |
| `.../Shader.hpp`、`ShaderInfo.hpp` | `render/shader` 与 `description/shader` 分拆 | SPLIT |
| `.../LayoutContract.hpp` | `modules/function/render/shader/LayoutContract.hpp` | MOVE |
| `.../RenderRepresentation.hpp` | `modules/function/render/Representation.hpp` | MOVE |
| `.../ImportedMaterialDesc.hpp` | `engine/toolchain/model_import/ImportedMaterial.hpp` | MOVE |
| `.../MaterialGraphContract.hpp` | `engine/authoring/material` 或 `engine/toolchain/material` | MOVE |
| `.../Script.hpp` | `modules/function/script` 的脚本值协议 | MOVE/SPLIT |
| `modules/resource/spatial` | `modules/core/math` | MOVE/DELETE |
| `modules/resource/deployment` | `render/config` + `engine/game/deployment` | SPLIT/DELETE |
| `modules/resource/entity_scene` | `ecs/scene_format` + `engine/scene/package` | SPLIT/DELETE |
| `modules/resource/spatial3d_scene` | `engine/spatial3d` | MOVE/DELETE |
| `modules/resource/classic_mesh` | `render/standard/content` + `asset/codecs` | SPLIT/DELETE |
| `modules/resource/physics3d` | 历史先归 Resource；现行最终 owner 为 `ecs/physics3d` | SECONDARY MOVE/DELETE |
| `modules/resource/terrain` | 历史先归 Resource；现行最终 owner 为 `ecs/terrain` | SECONDARY MOVE/DELETE |
| `modules/resource/tilemap` | 历史先归 Resource；现行最终 owner 为 `ecs/tilemap` | SECONDARY MOVE/DELETE |

## 3. `description` 的严格准入合同

### 3.1 允许内容

```text
普通 struct / enum / strong ID
数组、字符串、向量等值字段
纯校验函数
与格式自身相关的 schema version
不持有线程、设备、文件、registry 的数据
```

### 3.2 禁止内容

```text
AssetManager / AssetRef 生命周期
Renderer Handle
World / Entity / Component
Scene Feature
Extension Requirement
Game Launch Policy
Editor Import State
Toolchain 中间流程状态
Runtime Capacity Planner
全局 Registry
```

### 3.3 目录只做源码组织，不自动变成组件

目标：

```text
description/
├── include/lux/description/
│   ├── Image.hpp
│   ├── Mesh.hpp
│   ├── Material.hpp
│   ├── Shader.hpp
│   ├── Animation.hpp
│   ├── Skeleton.hpp
│   ├── Physics.hpp
│   ├── Terrain.hpp
│   └── Tilemap.hpp
├── src/
│   ├── image/
│   ├── shader/
│   └── validation/
└── CMakeLists.txt
```

默认只有一个公共目标：

```cmake
lux::description
```

若编译时间需要拆分，使用私有 Object Library：

```cmake
add_library(description_shader_obj OBJECT ...)
target_sources(lux_description PRIVATE
    $<TARGET_OBJECTS:description_shader_obj>)
```

外部用户不应为了使用 `TerrainTile` 再理解 `terrain_content` 安装组件。

## 4. Description 文件处理

### 4.1 保留的纯值类型

保留并清理：

```text
AnimationClip
Skeleton
Mesh
Model
Vertex
Texture
TextureAtlas
Material enums
Tilemap pure values
```

清理要求：

- 移除 `lux/engine/...` Include Prefix；
- 移除 Runtime Reflection 注解依赖；
- 将反射元数据生成清单放到消费者 sidecar；
- 不包含 Asset ID，除非资源描述本质需要跨 Asset 引用；优先使用逻辑索引或显式 `AssetId`；
- 不包含 Renderer 内部句柄；
- 不包含 Editor 专用字段。

### 4.2 `LayoutContract.hpp`

MOVE：

```text
modules/resource/description/include/lux/engine/description/LayoutContract.hpp
→ modules/function/render/shader/include/lux/render/shader/LayoutContract.hpp
```

原因：它定义 Descriptor Set、Binding、Update Frequency、Engine-shared Set 和 Render Feature Resource，是 Renderer 与 Shader Toolchain 的协议。

相关消费者改为共同依赖：

```text
lux::render_shader_contract
```

或者将其作为 `lux::render` 的小型 header-only 子目标。不得让 Asset、Animation、Navigation 因此依赖 Render。

### 4.3 `RenderRepresentation.hpp`

MOVE 到 Render：

```text
lux::rdesc::RenderRepresentationId
→ lux::render::RepresentationId
```

将当前对 `extension_abi/StableId.hpp` 的 include 改为直接使用 `lux-cxx::StableNameId`。

标准 ID：

```cpp
inline constexpr auto classicMeshRepresentation =
    representationId("lux.render.geometry.classic_mesh");
```

属于 Render API，不属于 Extension ABI。

### 4.4 `ImportedMaterialDesc.hpp`

MOVE 到：

```text
engine/toolchain/model_import/include/lux/engine/toolchain/model_import/ImportedMaterial.hpp
```

它是 Assimp/Model Importer 与 Material Graph Converter 之间的中间结构；不应为了两个 Toolchain 消费者而下沉到 Resource。

若未来外部模型导入库确有使用需求，可再抽为独立 `lux-model-import` 公共库；不能提前污染 `description`。

### 4.5 `MaterialGraphContract.hpp`

按实际内容拆分：

```text
纯运行材质参数布局 → render/material
Authoring 节点图契约 → engine/authoring/material
Cooked 编译协议 → engine/toolchain/material
```

不得保留一个同时覆盖 Authoring Graph、Runtime Shader 与 Asset 的万能 Contract。

### 4.6 `Script.hpp`

纯 Script Signature/Value 迁往 `modules/function/script`。Asset 中只保存 Script Payload 与 Type ID；Description 不负责 Script Runtime Module。

## 5. Asset 公共层重新设计

### 5.1 删除闭合 `EAssetType`

当前 `EAssetType` 中央枚举阻止外部库添加新 Asset 类型，并混合 Authoring 与 Runtime 类型。替换为开放 ID：

```cpp
namespace lux::asset
{
    struct AssetTypeIdTag final {};
    using AssetTypeId =
        lux::cxx::StableNameId<AssetTypeIdTag>;
    using AssetTypeIdView =
        lux::cxx::StableNameIdView<AssetTypeIdTag>;

    [[nodiscard]] constexpr AssetTypeIdView
    assetType(std::string_view name) noexcept
    {
        return AssetTypeIdView{name};
    }
}
```

各领域定义自己的 ID：

```cpp
namespace lux::description::types
{
    inline constexpr auto texture =
        asset::assetType("lux.asset.texture");
    inline constexpr auto mesh =
        asset::assetType("lux.asset.mesh");
}
```

Authoring 类型放在 Authoring：

```cpp
inline constexpr auto materialGraph =
    asset::assetType("lux.authoring.material_graph");
```

### 5.2 `asset_id_t` 迁移

CREATE：

```cpp
namespace lux::asset
{
    using AssetId = uuids::uuid;
    inline constexpr AssetId nullAssetId{};
}
```

兼容：

```cpp
using asset_id_t [[deprecated("use AssetId")]] = AssetId;
```

新代码禁止 `_t` 风格。

### 5.3 删除 `LuxAsset : LuxObject`

当前 `LuxAsset`：

- 继承 Runtime Reflection `LuxObject`；
- 以虚函数暴露 `void* rawData()`；
- 依赖闭合 `EAssetType`；
- 同时携带元数据、可变 payload、CPU unload 状态。

目标公共 Asset 层不需要对象继承树。建立文件与解码结果模型：

```cpp
namespace lux::asset
{
    struct AssetHeader final
    {
        AssetId id;
        AssetTypeId type;
        std::uint32_t schemaVersion{};
        std::uint64_t payloadBytes{};
    };

    class DecodedAsset final
    {
    public:
        DecodedAsset() = default;

        template<class T>
        static DecodedAsset make(
            AssetId id,
            AssetTypeId type,
            std::unique_ptr<T> value);

        [[nodiscard]] AssetId id() const noexcept;
        [[nodiscard]] AssetTypeIdView type() const noexcept;

        template<class T>
        [[nodiscard]] const T* get(AssetTypeIdView expected) const noexcept;

    private:
        AssetId id_{};
        AssetTypeId type_;
        std::unique_ptr<void, void(*)(void*)> value_;
    };
}
```

`DecodedAsset` 只表达一次解码结果；缓存、共享观察与驱逐由 Engine `AssetStore` 决定。

### 5.4 Codec API

```cpp
namespace lux::asset
{
    struct DecodeRequest final
    {
        AssetHeader header;
        std::span<const std::byte> payload;
    };

    class Codec
    {
    public:
        virtual ~Codec() = default;
        virtual AssetTypeIdView type() const noexcept = 0;

        virtual expected<DecodedAsset, DecodeError>
        decode(const DecodeRequest&) const = 0;

        virtual expected<std::vector<std::byte>, EncodeError>
        encode(const DecodedAsset&) const = 0;
    };

    class CodecRegistry final
    {
    public:
        expected<void, CodecRegistryError>
        add(std::unique_ptr<Codec> codec);

        const Codec* find(AssetTypeIdView type) const noexcept;
    };
}
```

若 Codec 需要模板化以避免 type erasure，可提供 `TypedCodec<T>` adapter，但注册表对外仍保持开放 Type ID。

### 5.5 Provider 与 VFS

Provider 负责 `asset_id_t`/`VirtualPath` 到 opaque bytes 的字节来源，不负责解析、对象缓存、驻留或引用计数。现有 `IAssetProvider` 与 `AssetVfs` API 保持，但公共头迁入 `asset/storage`。

`AssetManager + AssetRef` 是唯一引用账本。Scene Section 等非驻留记录可以复用 Provider/VFS 读取字节，但不注册到 AssetManager，不创建 AssetRef。

### 5.6 Pak

保留 LUXPAK v2 wire。`PakAssetProvider`、writer 和 inspector 统一迁入 `asset/storage/pak`，并作为公共 Asset SDK API。公开存储面只表达：

```text
AssetId
AssetHeader
Provider
Reader
Hash
```

不得依赖 Engine 产品语义，Writer 也不使用 `EAssetType`/Catalog 解释 magic。Toolchain 保留 source 扫描、auxiliary 剔除、Catalog 策略、冲突诊断和发布组合，不得 include Asset `pinclude`。

## 6. `AssetManager` 上移

MOVE：

```text
modules/resource/asset/src/core/AssetManager.cpp
modules/resource/asset/include/.../AssetManager.hpp
→ engine/assets/src/AssetStore.cpp
→ engine/assets/include/lux/engine/assets/AssetStore.hpp
```

目标职责：

```text
已解码对象缓存
Asset Handle / generation
引用与驱逐
异步 load 协调
内存预算
主线程状态提交
Asset loaded/removed facts
```

目标接口：

```cpp
namespace lux::engine::assets
{
    class AssetStore final
    {
    public:
        LoadTicket load(asset::AssetId id);
        AssetHandle find(asset::AssetId id) const noexcept;
        void release(AssetHandle);
        void setBudget(MemoryBudget);
        CloseTask close();

    private:
        asset::Vfs& vfs_;
        asset::CodecRegistry& codecs_;
        execution::Executor& executor_;
    };
}
```

注意：这是 Engine 类型，因此 Include Prefix 保留 `lux/engine/assets` 是合理的。

## 7. 内置 Asset 领域组织

单一 `lux::engine::resource::asset` target 保持不变，但源码、头和测试按资产领域族共同组织：

```text
modules/resource/asset/
├── include/lux/engine/resource/asset/
│   ├── ...资产核心接口与 AssetCodecCatalog.hpp
│   ├── texture/ material/ mesh/ model/
│   ├── animation/ shader/ script/
│   └── storage/{AssetProvider,AssetVfs,VirtualPath,pak/...}.hpp
├── pinclude/lux/engine/resource/asset/
│   ├── detail/...
│   ├── <domain>/*DescriptionCodec.hpp
│   └── storage/pak/PakCodec.hpp
├── src/
│   ├── <domain>/...
│   └── storage/...
└── test/<domain-or-storage>/...
```

`TextureCodec/ModelCodec` 改名为 `TextureSerDeser/ModelSerDeser`。跨领域组合继续使用 `runtimeAssetCodecCatalog()`，不新建第二套 registry。`BuiltinAssetIds.hpp` 不是通用 Asset 合同，迁入 `engine/content`。

## 8. 其他 Resource 目录迁移

### 8.1 `deployment`

SPLIT：

```text
RuntimeCapacity*
    → modules/function/render/config/RenderCapacity.hpp
      仅保留真正的 GPU/Renderer capacity

RuntimeLaunchManifest*
    → engine/game/deployment/GameManifest.hpp
```

`GameManifest` 应包含：

```text
title
content packs
boot scene/package
required extensions
product options
```

不再出现 `engine_pak`；改为语义中立：

```text
base_pack
game_pack
```

Extension Entry 使用 `engine/extensions/api`，因此该 Manifest 必须位于 Engine。

### 8.2 `entity_scene`

SPLIT 详见文档 05：

```text
EntitySection wire image / persistent IDs / component schema
    → ecs/scene_format

RequiredExtension / SceneContribution / startup package
    → engine/scene/package
```

### 8.3 `spatial3d_scene`

MOVE：

```text
Spatial3DSceneCatalog
Spatial3DResidencyCapacity
lux.spatial3d.* demand channels
→ engine/spatial3d/world_partition
```

它是 Lux Engine World Partition Feature 配置，不是公共 Resource。

### 8.4 `classic_mesh`

拆为：

```text
通用 Mesh/Instance 值
    → description/mesh

标准 Renderer 的 batch content
    → render/standard/content

对应 Asset Codec
    → asset/codecs/render_standard
```

`kClassicMeshBatchContentTypeName` 属于标准 Renderer，不应成为 Resource 根组件。

### 8.5 `terrain`、`tilemap`、`physics3d`

数据值与 Codec 分离：

```text
TerrainTileBlobV1            → description/terrain
TerrainTileCodec             → asset/codecs/terrain

TilemapChunkBlobV1           → description/tilemap
TilemapChunkCodec            → asset/codecs/tilemap

StaticColliderBatch3D        → description/physics
StaticColliderBatch3DCodec   → asset/codecs/physics
```

它们不再出现在 `install_components(lux-engine-resource ...)` 的一级列表中。

## 9. CMake 目标

目标 `modules/resource/CMakeLists.txt`：

```cmake
add_subdirectory(description)
add_subdirectory(asset)

install_components(
    PROJECT_NAME lux-resource
    VERSION      1.0.0
    NAMESPACE    lux
    COMPONENTS
        description
        asset
)
```

`description/CMakeLists.txt` 当前仍保持既有 `extension_abi` 闭包；本轮只归一化 Asset
目录和组件，不推进 Extension ABI/M0 的下沉或删除。

`asset/CMakeLists.txt` 仅公开声明现有有效闭包（Core Math/Meta/Serialization、
Description、stduuid 与 lux-cxx 的 compile_time/memory），算法、核心工具和反射运行时
保持 PRIVATE；不链接 ECS 或 Engine Runtime。

## 10. 文件格式兼容迁移

### 10.1 Asset Header

先保持 wire layout，建立新外观：

```cpp
struct LegacyAssetInfoV2;
struct AssetHeaderV3;
```

读取路径：

```text
probe magic/version
→ decode legacy header
→ convert to AssetHeader
→ select open AssetTypeId
```

写入路径在单独格式升级 PR 切换。

### 10.2 `EAssetType` 转换表

建立只读 legacy adapter：

```cpp
expected<AssetTypeId, LegacyTypeError>
fromLegacyAssetType(EAssetType);

expected<EAssetType, LegacyTypeError>
toLegacyAssetType(AssetTypeIdView);
```

仅旧格式 Codec 使用；业务代码不得继续 switch `EAssetType`。

### 10.3 Golden Tests

为每种现有资产保存至少一个固定二进制样本：

```text
texture
mesh
model
material
material instance
shader
script
skeleton
animation clip
texture atlas
flow graph
entity scene / section
```

新 Reader 必须读取全部旧样本。

## 11. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| RES-01 | 建立新 `lux::description`、`lux::asset` Alias 和新 Include | 无 wire 变化 |
| RES-02 | Stable `AssetId/AssetTypeId` 与 legacy adapter | 旧资产全可读 |
| RES-03 | CodecRegistry 开放化，去中央 switch | 标准 Codec 测试通过 |
| RES-04 | `LuxAsset` 消费者迁到 DecodedAsset/领域值 | 无 `rawData()` 新调用 |
| RES-05 | `AssetManager → engine/assets/AssetStore` | Modules SDK 无缓存语义 |
| RES-06 | `spatial → math`、Description 文件归位 | Resource 依赖闭包收窄 |
| RES-07 | Terrain/Tilemap/Physics/ClassicMesh 合并到 Description+Codecs | 一级组件删除 |
| RES-08 | Deployment/EntityScene/Spatial3DScene 上移 | Resource 无 Engine 协议 |
| RES-FINAL | 删除旧 targets、旧 include、`EAssetType` 业务使用 | Resource 只剩两个一级语义 |

## 12. 验收闸门

- [ ] `modules/resource` 根只有 `description` 与 `asset` 两个一级目录。
- [x] `lux::description` 不依赖 Extension ABI、ECS、Engine、Editor。
- [ ] `lux::asset` 不定义 `AssetManager`。
- [x] `lux::asset` 不继承 `LuxObject`。
- [ ] 新 Asset 类型无需修改中央 enum。
- [ ] `RuntimeLaunchManifest` 不在 Resource。
- [ ] `EntitySceneManifest` 的 Engine Feature 部分不在 Resource。
- [ ] `LayoutContract` 不在 Description。
- [ ] `ImportedMaterialDesc` 不在 Description。
- [ ] `resource/spatial`、`classic_mesh`、`terrain`、`tilemap`、`physics3d` 一级 targets 已删除。
- [ ] 旧 `.luxasset` Golden Files 全部可读。
- [x] Asset-only 外部样例不链接 ECS/Engine/Reflection Registry。


---

# Function 公共模块重构

> **2026-08-21 依赖裁决：** Function Animation 只消费 Description 中的 Skeleton/AnimationClip 值与 Core Math，不应通过 Resource Asset 获得这些类型。本轮将删除该陈旧 PUBLIC Asset 依赖，不改变采样 API 或 wire。

> 纯化 Render、Input、Animation、Navigation、Script 与 UI 的公共闭包，使其真正可脱离 ECS 和 Engine 使用

**执行文档 04 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Render、Input、Animation、Navigation、Script、UI 与公共 SDK 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 关联裁决：** Function 仍只依赖通用 Resource 契约；本文出现的 “AssetStore” 应理解为现有公共 `AssetManager` 的上层使用者，不再要求创建 Engine AssetStore。详见 `ADR-20260820_SceneAsset与Resource边界.md`。


## 1. 目标与边界

`modules/function` 仍是公共 SDK 的领域能力层，但不再作为“所有非 ECS 代码”的泛化容器。每个模块必须有清晰外部产品价值和最小依赖闭包。

| 当前目标 | 目标公开目标 | 核心处理 |
| --- | --- | --- |
| `render_client` | `lux::render` 或内部 protocol 子目标 | 移除 Meta、Deployment、Platform Common 的 PUBLIC 依赖 |
| `render_graph` | `lux::render_graph` | 保持设备无关；依赖 core/math/containers，不依赖 Platform Common |
| `render_vulkan` | `lux::render_vulkan` | 消费保留的 GAPI；Window Surface 作为叶子 Integration |
| `render_features` | `lux::render_standard` + 私有 feature object libs | Grid/Gizmo/Highlight 工具能力上移或可选 |
| `input` | `lux::input` | 物理输入值不依赖 Window Backend |
| `animation` | `lux::animation` | 只依赖 Description 和 Math |
| `navigation` | `lux::navigation` | 依赖 Math，不依赖 resource/spatial |
| `script_core` | `lux::script` | `ScriptHost → ScriptRuntime` |
| `script_lua` | `lux::script_lua` | 保留 LuaJIT/sol2 Backend |
| `script_native` | `lux::script_native` | 依赖 DynamicLibrary；与 Engine Extension ABI 无关 |
| `ui` | `lux::ui` + `lux::ui_imgui` | Panel/Widget 与 ImGui Backend 分离 |
| `ui_vulkan` | `lux::ui_render_vulkan` | 只做 UI draw data 到 Render/Vulkan Integration |

## 2. Render：公共 SDK 的主边界

### 2.1 目标分层

```text
modules/function/render/
├── api/             backend-neutral Renderer、Frame、View、Resource Handles
├── graph/           logical render graph
├── shader/          shader/layout contract
├── vulkan/          Vulkan backend 与内部 low-level wrappers
├── standard/        可选标准渲染能力集合
└── integrations/
    ├── window_vulkan/
    └── ui_vulkan/
```

公开目标：

```text
lux::render
lux::render_graph
lux::render_vulkan
lux::render_standard
```

内部目标可以更多，但不进入安装组件列表。

### 2.2 `render_client` 的处理

当前 `render_client` 包含大量 Operation Client、Frame/Control/Upload Session 与生成通信代理。保留其线程隔离和 Channel 设计，但对外表面重组为 Renderer API。

目标：

```cpp
namespace lux::render
{
    class Renderer;

    class Frame final
    {
    public:
        SceneView createView(...);
        void submit(RenderPacket);
        expected<void, FrameError> finish();
    };

    class UploadQueue final
    {
    public:
        UploadTicket upload(BufferUpload);
        UploadTicket upload(ImageUpload);
    };
}
```

`RenderControlSession`、`RenderFrameSession`、`RenderUploadSession` 可作为内部实现类型；普通外部用户不应同时理解三套 Session 和每个 Operation Client。

### 2.3 删除 PUBLIC Meta 依赖

当前 `render_client` PUBLIC 链接 `core::meta`，主要用于生成通信 Operation。施工：

1. 生成器在 Build-time 读取声明；
2. 生成 `.ops.hpp/.cpp`；
3. Runtime 目标只编译生成结果；
4. 安装 Config 不查找 Meta Generator；
5. 若生成结果需要 `TypeId`，使用轻量 `lux-cxx::type_hash` 或显式 Operation ID。

禁止：

```cmake
target_link_libraries(render_client PUBLIC meta)
```

### 2.4 删除 Deployment 依赖

`RuntimeCapacity` 拆分：

- GPU/Renderer Capacity 类型迁入 `render/config`；
- Game Product Capacity Request 留在 Engine Game Manifest；
- Renderer 构造接收 `RenderConfig`，不读取 Game Manifest。

目标：

```cpp
struct RenderCapacity final
{
    std::uint64_t maximumInstances{};
    std::uint64_t geometryBytes{};
    std::uint32_t bindlessTextures{};
};

struct RenderConfig final
{
    RenderCapacity requested{};
    Validation validation{Validation::enabled};
};
```

### 2.5 `render_graph`

当前目标只编译 `DependencyAnalyzer.cpp`，保持设备无关。修改：

```cmake
target_link_libraries(render_graph
    PUBLIC
        lux::cxx::container
        lux::cxx::compile_time
)
```

移除 `platform::common`。`Size2D`、Format 等精确依赖改为 `lux::math` 或 `lux-cxx`。

### 2.6 `render_vulkan` 消费保留的 GAPI

按 ADR-20260821，GAPI 继续作为公共 Platform Vulkan wrapper SDK。`render_vulkan` 可以链接并使用它，但不迁移其目录、target、component 或 namespace，也不在本重构中强制把 GAPI wrapper 与 Render 内部 handle 合并为一种公共对象模型。

### 2.7 Window Surface Integration

`render_vulkan` 核心只接收抽象 Surface Factory 或已创建 Surface：

```cpp
struct SurfaceSource
{
    platform::NativeWindowHandle window;
};

expected<Surface, SurfaceError>
createVulkanSurface(VulkanInstance&, SurfaceSource);
```

目标依赖：

```text
render_vulkan core       不依赖 window_glfw
render_vulkan_window     依赖 window + render_vulkan
```

### 2.8 `render_features` 收敛

当前一个目标包含 Deferred、Forward、Grid、Gizmo、Highlight、Point Cloud、Terrain、Water、Shadow、Skybox、Postprocess 等大量实现。

施工分组：

| 分组 | 内容 | 安装策略 |
| --- | --- | --- |
| `standard_core` | View、Depth、Forward/Deferred 基础、Tonemap | 合入 `lux::render_standard` |
| `standard_lighting` | Light、Shadow、BRDF、Skybox | 合入 `lux::render_standard` |
| `standard_geometry` | Mesh、Skinning、Material | 合入 `lux::render_standard` |
| `render_tooling` | Grid、Gizmo、Highlight、Picking Overlay | 默认不装；Editor/Tooling 使用 |
| `render_pointcloud` | Point Cloud Feature | 确有独立用户时公开可选 |
| `render_terrain` | Terrain/Water | 确有独立用户时公开可选 |
| `render_streaming` | Feedback、LOD、Cull | 可作为 standard 内部 |

先使用 Object Library 拆编译单元，再决定公开组件，避免组件爆炸。

### 2.9 Renderer 主对象

公共 API：

```cpp
namespace lux::render
{
    class Renderer final
    {
    public:
        static expected<Renderer, OpenError>
        open(RenderConfig, Backend, Surface);

        Frame beginFrame(FrameInfo);
        UploadQueue& uploads() noexcept;
        SceneView createSceneView(SceneViewConfig);
        OffscreenView createOffscreenView(OffscreenConfig);

        CloseTask close();

        Renderer(Renderer&&) noexcept;
        Renderer& operator=(Renderer&&) noexcept;
        ~Renderer();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

公共 `Renderer.hpp` 不包含 Vulkan 类型；`Backend` 可由 Vulkan 工厂产生。

## 3. Input 解耦

### 3.1 当前问题

`input` 的 Action Mapping 逻辑是通用的，但 PUBLIC 依赖 `platform::window`，并因此向消费者泄漏 GLFW。

### 3.2 目标结构

```text
modules/function/input/
├── include/lux/engine/input/
│   ├── Input.hpp
│   ├── InputSnapshot.hpp
│   ├── PhysicalInput.hpp
│   └── Action/Mapping/Context headers
└── src/
    ├── Input.cpp
    ├── ActionMapper.cpp
    ├── BindingIdAllocator.cpp
    └── platform/{glfw|android}/InputPlatform.cpp
```

目标值类型：

```cpp
enum class EKey;
enum class EMouseButton;
struct InputSnapshot;
```

公开领域对象：

```cpp
class Input final
{
public:
    void sample(window::LuxWindow&);
    void evaluate(float dt, bool accept_keyboard, bool accept_pointer);
    ActionMapper& mapper() noexcept;
    InputActionRegistry& actionRegistry() noexcept;
    InputContextStack& contexts() noexcept;
};
```

Input 只有一个 target。CMake 在配置期只选择一个私有平台实现；不创建 Adapter target、backend interface、factory 或注册表。`lux::input` 公共头不 include `<GLFW/glfw3.h>`。

### 3.3 对外对象命名

`Input` 是平台宿主使用的统一所有权对象：

```cpp
class Input final
{
public:
    void sample(window::LuxWindow&);
    void evaluate(float dt, bool accept_keyboard = true,
                  bool accept_pointer = true);
    ActionMapper& mapper() noexcept;
    InputActionRegistry& actionRegistry() noexcept;
    InputContextStack& contexts() noexcept;
};
```

`InputActionRegistry` 只保留 `ActionMapper` 内部唯一实例。`ActionMapper` 仍可在 headless Preview 与低层 Runtime 中单独使用，但 GameApplication 不得再维护第二份 Registry。

## 4. Animation 解耦

历史基线中 `animation` PUBLIC 依赖 `asset_core`；`d1ead288` 已改为直接依赖
Description 与 Math。目标函数直接操作 Description：

```cpp
Pose sample(
    const description::Skeleton&,
    const description::AnimationClip&,
    AnimationTime,
    Scratch&);
```

ECS `AnimationSystem` 与现有 Engine Runtime 资产编排负责：

```text
AssetHandle → Skeleton/AnimationClip value → lux::animation::sample()
```

MODIFY：

```text
modules/function/animation/CMakeLists.txt
```

移除：

```text
lux::engine::resource::asset
```

新增：

```text
lux::engine::resource::description
lux::engine::core::math
```

## 5. Navigation 解耦

当前 `navigation` 依赖 `resource::spatial`。Spatial 值迁入 Math 后：

```cmake
target_link_libraries(navigation
    INTERFACE
        lux::math)
```

Detour3D Backend 保持独立：

```text
lux::navigation
lux::navigation_detour3d
```

ECS Navigation Region、Agent Component 与 System 留在 `ecs/navigation`。

## 6. Script 词汇与边界

### 6.1 `ScriptModule` 保留

这是语言 Runtime 加载单元，与 Engine Extension 无关。

### 6.2 `ScriptHost → ScriptRuntime`

当前对象实际负责 Backend 注册、Module 加载、Function Lookup 与 Invoke，符合 Runtime 语义。

RENAME：

```text
ScriptHost.hpp/.cpp → ScriptRuntime.hpp/.cpp
ScriptHostImpl      → ScriptRuntime::State
```

目标：

```cpp
class ScriptRuntime final
{
public:
    void addBackend(std::unique_ptr<Backend>);
    expected<ModuleHandle, LoadError> load(...);
    expected<void, InvokeError> invoke(FunctionHandle, CallFrame&);
    void unload(ModuleHandle);
};
```

改掉返回 `kInvalidModule` 与 `lastError()` 的隐式错误通道，使用 `expected`。

### 6.3 Native Script 与 Engine Extension 分开

```text
script_native
    加载脚本编译产物
    使用 Script ABI
    依赖 DynamicLibrary

engine/extensions
    加载 Lux Engine Extension
    使用 Extension ABI
    注册 ECS/Scene/Render/Execution 能力
```

任何头文件不得同时 include 两种 ABI。

## 7. UI 拆分

### 7.1 当前问题

基础 `ui` 目标 PUBLIC 链接 ImGui GLFW 与 Vulkan，并包含 `SceneViewportPanel`；这使“使用 Panel”自动获得窗口与 Vulkan 依赖。

### 7.2 目标结构

```text
modules/function/ui/
├── core/
│   ├── Panel.hpp
│   ├── Widget.hpp
│   ├── Layout.hpp
│   └── Command.hpp
├── imgui/
│   ├── ImGuiContext.hpp
│   └── ImGuiWidgets.cpp
├── integrations/
│   ├── imgui_glfw/
│   └── imgui_render_vulkan/
└── CMakeLists.txt

engine/editor/
└── viewport/
    └── SceneViewport.cpp
```

公开目标：

```text
lux::ui
lux::ui_imgui
lux::ui_imgui_glfw
lux::ui_render_vulkan
```

### 7.3 `UISystem`

若它拥有 ImGui Context 与每帧绘制，RENAME：

```text
UISystem → UI
```

因为 `System` 只保留给 ECS。

### 7.4 `SceneViewportPanel`

MOVE：

```text
modules/function/ui/src/SceneViewportPanel.cpp
modules/function/ui/include/.../SceneViewportPanel.hpp
→ engine/editor/src/viewport/SceneViewport.cpp
```

它知道 Lux Scene/Render View/Editor selection，不是通用 UI。

### 7.5 Backend 依赖

```cmake
target_link_libraries(ui PUBLIC /* no GLFW, no Vulkan */)
target_link_libraries(ui_imgui PUBLIC imgui::core)
target_link_libraries(ui_imgui_glfw PRIVATE glfw)
target_link_libraries(ui_render_vulkan PRIVATE lux::render_vulkan)
```

## 8. Shader 与 Codegen

Shader Layout、Pass Params、通信 Operation Codegen 都属于 Render Build Tooling。建立：

```text
modules/function/render/shader/
cmake/Codegen/RenderOperations.cmake
cmake/Codegen/ShaderParams.cmake
```

生成器可由 Toolchain Profile 构建，但生成产物目标不 PUBLIC 链接生成器。

`render_features` 中对 `include_component_cmake_scripts(meta)` 的依赖改为通用 Codegen CMake API，避免公共 Render 包需要 Engine Meta Component。

## 9. CMake 修改总表

| 当前文件 | 修改 |
| --- | --- |
| `modules/function/CMakeLists.txt` | 显式子目录；安装包按领域拆分 |
| `render/client/CMakeLists.txt` | 删除 Meta、Deployment、Platform Common 的 PUBLIC 依赖 |
| `render/graph/CMakeLists.txt` | 精确依赖 Core/Math/Container |
| `render/vulkan/CMakeLists.txt` | 消费保留的 GAPI；Surface Integration 分离 |
| `render/features/CMakeLists.txt` | Object Library 分组；工具 Feature 移出默认标准包 |
| `input/CMakeLists.txt` | 不 PUBLIC 链接 Window |
| `animation/CMakeLists.txt` | 依赖 Description，不依赖 Asset Core |
| `navigation/core/CMakeLists.txt` | 依赖 Math |
| `script/core/CMakeLists.txt` | `ScriptRuntime` API 与 expected 错误 |
| `ui/CMakeLists.txt` | Core/ImGui/GLFW/Vulkan/Editor Viewport 拆分 |

## 10. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| FUNC-01 | 新公共 Alias 与 Include Prefix | 旧 API 兼容 |
| RENDER-01 | Render Config 与 Deployment 解耦 | render_client 无 deployment |
| RENDER-02 | Build-time Meta 依赖移出 Runtime Link | 安装包无 generator |
| RENDER-03 | GAPI 保留裁决 | 迁移/删除目标 SUPERSEDED，公共 GAPI 保持可链接 |
| RENDER-04 | Window Surface Integration 分离 | render_vulkan core 无 GLFW |
| RENDER-05 | features Object Library 分组 | 行为与 shader 输出不变 |
| INPUT-01 | Snapshot 与 Window Backend 分离 | input-only 样例无 GLFW |
| ANIM-01 | Animation 直接依赖 Description | 无 AssetStore |
| NAV-01 | Navigation 改依赖 Math | resource/spatial 删除 |
| SCRIPT-01 | ScriptRuntime 与 expected | Native/Lua 测试通过 |
| UI-01 | UI Core/ImGui/Backend 拆分 | UI Core 无 GLFW/Vulkan |
| UI-02 | SceneViewport 迁入 Editor | modules/ui 无 Scene 类型 |
| FUNC-FINAL | 删除旧 targets/include | Modules SDK 闭包纯净 |

## 11. 验收闸门

- [ ] `lux::render` 公共头无 ECS、Scene、Editor 类型。
- [ ] `lux::render` 安装 Config 无 Meta Generator、Deployment、Extension ABI。
- [ ] `lux::render_vulkan` 核心不依赖 GLFW。
- [x] `platform/gapi` 按保留 ADR 继续作为公共组件存在。
- [ ] `render_graph` 可在无 Vulkan SDK 的测试配置中编译。
- [ ] `lux::input` 可在无 GLFW 的配置中编译。
- [ ] `lux::animation` 不依赖 Asset Core/AssetStore。
- [ ] `lux::navigation` 不依赖 Resource Spatial。
- [ ] Script ABI 与 Extension ABI 无共享头。
- [ ] `lux::ui` 无 GLFW/Vulkan 依赖。
- [ ] `SceneViewportPanel` 不在 modules。
- [ ] Tooling Feature 不自动进入标准 Renderer 公共闭包。


---

# ECS 内核、序列化与 Scene Format 重构

> 让 ECS 重新成为真正独立的实体组件系统层，并把 Entity Scene 格式与 Engine Scene Package 分开

**执行文档 05 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | ECS Kernel、Component Reflection、Scene Serialization、Runtime Entity Loading 与 Toolchain Cooker 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** ECS Scene Format 继续拥有 LXES/Persistence 原始记录，不直接拥有 `entt::registry` 的文件镜像。Terrain、Tilemap 与 Physics3D 场景 Payload 的值、Codec 和 wire tests 归各自 ECS 领域；Engine `SceneDescription/SceneAsset` 负责 LXSC。详见 `ADR-20260820_SceneAsset与Resource边界.md`。

> **2026-08-21 裁决更新：** ECS 不拥有、不包含 Engine 内置资产身份。Render Residency 的 fallback material ID 由 Engine Runtime 装配时注入；nil 明确表示不请求默认材质。

> **2026-08-21 Registry/资产需求裁决：** Registry 原样归 ECS Core，Core Meta 不再链接 EnTT；`AssetLoadFn` 直接删除，不创建空泛 `ecs/assets integration`。Animation/Script 的资产请求由 Engine Runtime integration 显式使用 `AssetClient`，ECS 系统只消费 ready 数据。

> **2026-08-21 实施状态：** Registry/资产需求裁决已完成。ECS Core installed consumer 不导入 Resource Asset；Animation Resolver 与 Script request system 均由 Runtime integration 私有拥有，ECS production 不执行同步资产 IO。

> **2026-08-21 Component Archive 裁决：** Reflection-driven tagged-property archive 整体归 `ecs/serialization` 的 `component_archive` component；Core 只保留 byte Archive/NameTable。不建立 RegistryArchive，Unknown Component schema 在 Authoring/Toolchain/Runtime 均拒绝。详见 `ADR-20260821_CoreSerialization与ECSComponentArchive边界.md`。

> **2026-08-21 Component Archive 实施状态：** `d1ead288` 已建立详细 expected/limits、compatible/exact reader 与 UUID annotation 语义；LXWA/LXES/Persistence/L3SC/Infinite2D owner 契约和 installed consumer 均通过。


## 1. 当前问题

`ecs/CMakeLists.txt` 对自身的定义是正确的：知道 Entity/Component 的代码属于 ECS，Function 保持 ECS-free。但 `ecs/core` 当前 PUBLIC 依赖多个错误下沉的模块，形成形式无环、语义倒置的结构。

| 当前依赖用途 | 目标 | 动作 |
| --- | --- | --- |
| `core::meta` for EntityRegistry | `ecs/core` owns Registry directly | REMOVE dependency |
| `core::extension_abi` for ComponentSchemaId | `ecs::ComponentSchemaId` | REMOVE dependency |
| `resource::asset_core` for AssetLoadFn | `ecs/integration/assets` | REMOVE from kernel |
| `resource::entity_scene` for persistent identity | `ecs/scene_format` | SPLIT |
| `resource::spatial` for Position | `lux::math` | REPLACE |

## 2. ECS Core 最终合同

ECS Kernel 只负责：

```text
Entity / Registry
World
ComponentSchema
ComponentTypeCatalog
ISystem
Schedule / ScheduleBuilder / topology
SceneServices 的装配期机制
Command Buffer / barrier
Hierarchy / Name 等真正通用组件
```

它不负责：

```text
Asset loading
Game Scene Package
Extension loading
Render backend
Window
Editor
World Partition
Script backend implementation
```

目标依赖：

```text
EnTT
lux-cxx algorithm/compile_time/container
lux::math
可选 lux::serialization 基础
```

## 3. Entity Registry 回迁 ECS

### 3.1 MOVE

```text
modules/core/meta/include/lux/engine/meta/LuxObject.hpp
modules/core/meta/include/lux/engine/meta/RegistryMemoryResource.hpp
modules/core/meta/src/RegistryMemoryResource.cpp
→ ecs/core/include/lux/ecs/Registry.hpp
→ ecs/core/include/lux/ecs/RegistryMemory.hpp
→ ecs/core/src/Registry.cpp
→ ecs/core/src/RegistryMemory.cpp
```

### 3.2 命名

```text
lux::meta::entity_id          → lux::ecs::Entity
lux::meta::EntityRegistry     → lux::ecs::Registry
lux::meta::EntityHandle       → lux::ecs::EntityHandle
lux::meta::EntityObject       → 删除或 lux::ecs::EntityObject
LuxObject                     → 删除
```

推荐不保留 `EntityObject` OO wrapper，业务代码直接使用：

```cpp
Entity entity = registry.create();
registry.emplace<Transform>(entity, ...);
```

若外部 API 确需 RAII Entity，可建立窄 `OwnedEntity`，但不得作为所有 Asset/Reflection 对象的共同基类。

## 4. Component Schema ID 归 ECS

### 4.1 CREATE

```text
ecs/core/include/lux/ecs/ComponentSchemaId.hpp
```

```cpp
namespace lux::ecs
{
    struct ComponentSchemaIdTag final {};
    using ComponentSchemaId =
        lux::cxx::StableNameId<ComponentSchemaIdTag>;
    using ComponentSchemaIdView =
        lux::cxx::StableNameIdView<ComponentSchemaIdTag>;

    [[nodiscard]] constexpr ComponentSchemaIdView
    componentSchema(std::string_view name) noexcept
    {
        return ComponentSchemaIdView{name};
    }
}
```

### 4.2 MODIFY

所有 `ComponentSchemaDescriptor`、`ComponentTypeCatalog` 与 Scene Format 使用该类型。

DELETE ECS 对：

```text
lux::engine::core::extension_abi
lux::extensions::ContributionId
```

的依赖。

> **实施状态：** ECS 直接依赖已清零；`c56efbc4` 又删除了旧 Core component 与通用
> `ContributionId` 定义。上面的名称仅保留为历史删除合同。

## 5. Asset 依赖移出 Kernel

当前 `ecs/core` 因 `AssetLoadFn` 等类型 PUBLIC 链接 `asset_core`。施工：

### 5.1 核心层删除

从 `ecs/core` 删除：

```text
AssetLoadFn
AssetManager pointer/reference
Asset Load callback
Asset-specific SceneServices
```

### 5.2 建立 Integration

CREATE：

```text
ecs/integration/assets/
├── CMakeLists.txt
├── include/lux/ecs/integration/assets/
│   ├── AssetResolver.hpp
│   └── AssetComponentLoader.hpp
└── src/
```

接口使用 Engine-neutral port：

```cpp
namespace lux::ecs::integration
{
    class AssetResolver
    {
    public:
        virtual ~AssetResolver() = default;
        virtual AssetTicket request(asset::AssetId) = 0;
    };
}
```

Engine `AssetStore` 提供 Adapter。ECS Kernel 不知道缓存实现。

## 6. Reflection Sidecar 重构

### 6.1 类型所有者原则

每个组件目标拥有其组件描述与生成清单：

```text
ecs/core          → Name、Hierarchy、真正共享组件
ecs/transform     → Transform2D/3D
ecs/render        → Render components
ecs/physics3d     → Physics components
ecs/navigation    → Navigation components
```

不能把多个领域组件塞入 `ecs_meta` 只因方便。

### 6.2 Build-time 与 Runtime 分离

生成流程：

```text
组件头
→ build-time generator
→ generated metadata translation unit
→ 对应组件 sidecar/library
→ runtime catalog registration
```

ECS Core 只依赖生成后的 Runtime Reflection Interface，不依赖 generator executable。

### 6.3 Extension 动态加载

动态 Extension 加载后：

```text
ExtensionLoader 加载 DLL
→ Extension-owned metadata draft
→ validate against ComponentTypeCatalog
→ commit at safe point
→ ExtensionLease retained by installed schemas/systems
```

`ReflectionRegistry::drainPending()` 这种全局 pending chain 应逐步替换为显式 Draft；不能让动态库静态构造器隐式修改进程全局状态。

## 7. `SceneServices` 保留但内收

当前 `SceneServices` 的“装配期查询、运行时保存已解析依赖”方向正确。

规则：

- 只在 Scene/Feature 安装时 `find<T>()`；
- `ISystem::update()` 不得每帧查询；
- 必需服务解析后保存引用或专用 Client；
- 动态卸载服务使用 generation-aware `ServiceHandle<T>`；
- 不暴露 `Engine` 或通用 `get<T>()`。

可以更名：

```text
SceneServices → SceneServices（可保留）
```

因为其作用域和语义明确，不必为了统一命名而改。

## 8. Scene Format 与 Scene Package 拆分

### 8.1 当前混合

`modules/resource/entity_scene` 同时包含：

```text
Entity Section wire format
Persistent Entity identity
Component schema requirement
Required Extension
Scene Contribution
Startup sections
Generated section source
Persistence journal
```

前半属于 ECS，后半属于 Engine Scene 产品协议。

### 8.2 ECS Scene Format

CREATE：

```text
ecs/scene_format/
├── include/lux/ecs/scene_format/
│   ├── EntityId.hpp
│   ├── SectionId.hpp
│   ├── ComponentRecord.hpp
│   ├── SectionImage.hpp
│   ├── SectionCodec.hpp
│   └── Validation.hpp
├── src/
└── test/
```

目标内容：

```cpp
struct SectionImage final
{
    SectionId id;
    std::vector<EntityRecord> entities;
};

struct EntityRecord final
{
    PersistentEntityId id;
    std::vector<ComponentRecord> components;
};

struct ComponentRecord final
{
    ComponentSchemaId schema;
    std::uint32_t schemaVersion;
    std::vector<std::byte> payload;
};
```

该格式不知道：

```text
ExtensionId
SceneFeatureId
startup scene
game pack
render feature
world partition strategy
```

### 8.3 Engine Scene Package

CREATE：

```text
engine/scene/package/
├── include/lux/engine/scene/package/
│   ├── ScenePackage.hpp
│   ├── SceneFeatureRequest.hpp
│   ├── SectionSource.hpp
│   └── ScenePackageCodec.hpp
└── src/
```

目标：

```cpp
namespace lux::engine::scene::package
{
    struct RequiredExtension final
    {
        extensions::ExtensionId id;
        extensions::VersionRange version;
    };

    struct FeatureRequest final
    {
        scene::FeatureId id;
        std::uint32_t configVersion{};
        std::vector<std::byte> config;
    };

    struct ScenePackage final
    {
        SceneId id;
        std::vector<ecs::scene_format::SectionId> startupSections;
        std::vector<SectionSource> sections;
        std::vector<RequiredExtension> requiredExtensions;
        std::vector<FeatureRequest> features;
    };
}
```

### 8.4 Toolchain 依赖

`engine/toolchain/entity_scene` 改为：

```text
依赖 ecs/scene_format 生成 SectionImage
依赖 engine/scene/package 生成 ScenePackage
```

不再依赖一个 Resource 万能 Manifest。

## 9. Runtime Entity Loading 拆分

当前 `engine/runtime/entity_scene` 包含：

```text
EntityBatchDecoder
EntityBatchMaterializer
EntityBatchStager
EntitySceneCatalog
EntitySectionGeneratorCatalog
EntitySectionLoaderSystem
EntitySectionService
SectionBlobStore
StartupSectionSystem
```

目标归属：

| 当前类型 | 目标 |
| --- | --- |
| `EntityBatchDecoder` | `ecs/scene_format/SectionDecoder` |
| `EntityBatchMaterializer` | SUPERSEDED：纯 LXES image + Runtime `EntityBatchStager` 已提供 staged materialization |
| `EntityBatchStager` | `engine/scene/loading/SectionStager` |
| `EntitySceneCatalog` | `engine/scene/package/PackageCatalog` |
| `EntitySectionGeneratorCatalog` | `engine/scene/loading/SectionGeneratorCatalog` |
| `EntitySectionLoaderSystem` | `engine/scene/features/section_streaming/SectionLoaderSystem` |
| `EntitySectionService` | `engine/scene/loading/SectionLoader` |
| `SectionBlobStore` | `engine/scene/loading/SectionStore` |
| `StartupSectionSystem` | `engine/scene/features/startup_sections/StartupSectionSystem` |

解码纯函数下沉到 ECS Format；异步 I/O、AssetStore、Feature 与 System 留在 Engine。

## 10. Scene Feature 与 ECS System

### 10.1 所有权

```text
FeatureCatalog     保存 Feature Descriptor/Factory
Scene::Features    拥有已安装 Feature 状态
Schedule           唯一拥有具体 ISystem
SceneServices      拥有场景服务
```

Feature 安装事务：

```text
resolve feature dependencies
→ create unpublished services
→ create unpublished systems
→ validate service and schedule constraints
→ commit services
→ commit systems
→ publish active feature
```

失败必须全回滚。

### 10.2 Feature 不是 ECS Core

Physics3D、Navigation3D、Presentation3D 等 Feature 描述属于 Engine Scene Feature 层；具体 `PhysicsSystem` 等属于 ECS 领域目标。

## 11. `Schedule` 保留的机制

以下实现原则不可破坏：

- `Schedule` 保存 `std::unique_ptr<ISystem>`；
- System Handle 带 owner identity 与 generation；
- prerequisite/before/after 在拓扑编译时验证；
- 安装和移除只在安全点提交；
- `SystemUpdateContext` 只暴露 Registry、Command Writer、delta/tick；
- Command barrier 仍然唯一；
- 关闭按逆拓扑顺序。

重构只调整路径和外部装配，不将 Runtime/Asset/Extension 注入 `SystemUpdateContext`。

## 12. CMake 修改

### 12.1 `ecs/core/CMakeLists.txt`

目标 PUBLIC 依赖改为：

```cmake
target_link_libraries(core
    PUBLIC
        EnTT::EnTT
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::math
)
```

根据实际需要可加入纯 Reflection Runtime；移除：

```text
core::extension_abi
resource::asset_core
resource::entity_scene
resource::spatial
```

### 12.2 新目标

```text
lux::ecs::core
lux::engine::ecs::component_archive
lux::ecs::scene_format
```

`component_archive` 与 `scene_format` 可独立安装；不创建空泛 Asset Integration 或 RegistryArchive component。

## 13. 格式迁移

### 13.1 保持旧 LXES/LXSC 读取

第一阶段：

```text
Legacy EntityScene Codec
→ decode legacy manifest
→ split into ScenePackage + Section descriptors
```

第二阶段再写新格式。不能在目录迁移 PR 同时改变 wire image。

### 13.2 Golden Files

保存：

```text
空 Scene
单 Section
多依赖 Section
未知 Component
未知 Extension
压缩 Section
Generated Section
Persistence Journal
```

并验证：

```text
旧 Reader
新 Adapter
新 Reader（格式升级后）
```

## 14. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| ECS-01 | ComponentSchemaId 与 Registry 回迁 | Core 不依赖 extension_abi/meta entity |
| ECS-02 | Asset integration 移出 Kernel | Core 不依赖 Asset Core |
| ECS-03 | `resource/spatial → math` 调用迁移 | Core 不依赖 Resource Spatial |
| ECS-04 | 创建 `ecs/scene_format`，迁移纯 Codec | 旧 wire 可读 |
| ECS-05 | 创建 `engine/scene/package`，拆 Manifest | RequiredExtension 不在 ECS |
| ECS-06 | Runtime Entity Loading 按纯函数/Engine 拆分 | System 行为不变 |
| ECS-07 | Reflection static pending 改 Draft Commit | Dynamic load/unload 测试通过 |
| ECS-FINAL | 删除旧 Resource EntityScene targets | 依赖图符合合同 |

## 15. 验收闸门

- [ ] `ecs/core` 不依赖 `engine/*`。
- [x] `ecs/core` 不依赖 Extension ABI。
- [x] `ecs/core` 不依赖 AssetManager/AssetStore。
- [x] Entity Registry 位于 ECS。
- [ ] ComponentSchemaId 位于 ECS。
- [ ] ECS Scene Format 不包含 RequiredExtension 或 SceneFeature。
- [ ] Engine Scene Package 不进入 modules/resource。
- [ ] `Schedule` 仍唯一拥有 System。
- [ ] `SystemUpdateContext` 未新增 Runtime/Asset/Extension getter。
- [ ] 旧 Entity Scene/Section Golden Files 全部可读。
- [ ] 动态 Extension metadata 使用显式 Draft/Commit，不依赖隐式全局 pending chain。


---

# Engine 执行、资产、场景与扩展重构

> 纯化 Engine 的 Execution、Scene、ExtensionLoader 与产品适配边界，并复用公共资产机制

**执行文档 06 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Engine Execution、Asset、Scene、Extension、Render Integration、Spatial 与关闭协议负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 不建立 `engine/assets` 或 `AssetStore`。`engine/runtime/assets` 保留为现有公共 `AssetManager` 的异步编排适配器；`engine/scene` 收敛成拥有 `SceneDescription`、`SceneAsset` 与 `SceneAssetSerDeser` 的单一组件，不拥有 IO、异步执行或 Registry 生命周期。本文后续与此冲突的目标目录和接口由 `ADR-20260820_SceneAsset与Resource边界.md` 取代。

> **2026-08-21 裁决更新：** 新建的 `engine/content` 只拥有冻结内置资产 UUID、M_Missing 与色板，不是第二套资产系统。Runtime Render 在唯一装配点把 fallback material ID 注入 ECS Residency。Toolchain 只拥有 Pak cook/publish 策略，不访问 Resource 私有 Pak wire 实现。

> **2026-08-21 加载裁决：** `engine/runtime/assets` 通过既有 `AssetLoadService` 编排 IO、manager-less SerDeser decode 与主线程安装。Runtime packs/Scene Script integration 直接表达需求；不得向 ECS 注入裸加载函数或在 tick 中调用同步 `ensureAsset()`。

> **2026-08-21 Extension ABI 实施状态：** `c56efbc4` 已使 `engine/extensions/api` 成为 ABI v4 唯一实体 owner，Core owner 与通用 `ContributionId` 已删除。Authoring source DTO 在 Toolchain/Editor 边界显式转换；v4 registrar/draft/lease 名称保持冻结。


## 1. 目标目录

`engine/runtime` 当前承担“所有运行期代码”的物理聚合，但其子目录已经是独立领域。目标是去掉这一中间层，直接按语义组织：

```text
engine/
├── execution/
├── extensions/
│   ├── api/
│   ├── loader/
│   ├── registration/
│   └── reflection/
├── scene/
├── render/
│   └── scene_bridge/
├── spatial2d/
├── spatial3d/
├── spatial_partition/
├── logging/
├── game/
├── authoring/
├── toolchain/
└── editor/
```

| 当前目录 | 目标目录 | 核心处理 |
| --- | --- | --- |
| `engine/runtime/execution` | `engine/execution` | `AsyncRuntime → Executor`；保持 Engine 内部 |
| `engine/runtime/assets` | 保留 | 既有 `AssetManager` 的异步编排适配器，不复制资产系统 |
| `engine/runtime/extensions/loader` | `engine/extensions/loader` | `ExtensionModuleManager → ExtensionLoader` |
| `engine/runtime/extensions/contribution_host` | `engine/extensions/registration` + 各领域 Catalog | 拆除 `EngineExtensions` 聚合 |
| `engine/runtime/entity_scene` | 保留 Runtime loading 职责 | LXES 纯格式已归 `ecs/scene_format`；不并入 Scene Codec 组件 |
| `engine/runtime/scene/core` | 保留 Runtime 生命周期职责 | `engine/scene` 只提供 Scene Asset 数据与同步 SerDeser |
| `engine/runtime/scene/script` | `engine/scene/features/script` + `engine/game/Session` | Feature 与会话入口分开 |
| `engine/runtime/render/backend_host` | `modules/function/render` | `RenderBackendHost → render::Renderer` |
| `engine/runtime/render/scene` | `engine/render/scene_bridge` | ECS/Scene 到 Renderer 的集成 |
| `engine/runtime/frame` | 产品私有 `Game::Frames` / `Editor::Frames` | 不再安装为公共 Runtime Component |
| `engine/runtime/logging` | `engine/logging` | Engine async sink；公共 log 不依赖它 |
| `engine/runtime/packs` | `engine/scene/features/*` 或 `engine/game/standard_features` | 不再用 Pack 表示通用能力 |
| `engine/runtime/spatial*` | `engine/spatial2d`、`engine/spatial3d`、`engine/spatial_partition` | World Partition 与准备服务归领域 |

## 2. Execution：保留在 Engine，先纯化再评估

### 2.1 不立即下沉 modules

当前 Execution 具有：

```text
MainThreadMailbox
MainThreadScheduler
Engine close admission
Dynamic Operation Bundle
Extension Module Lease
owner-thread completion
```

这些是 Engine 语义。仅因为 Game 与 Editor 共用，不能把它下沉到公共 Core。

### 2.2 类型迁移

```text
lux::exec::AsyncRuntime          → lux::engine::execution::Executor
AsyncRuntimeBuilder              → ExecutorBuilder
AsyncOperation                   → Operation
AsyncOperationClient             → OperationClient
AsyncOperationBundle             → OperationBundle
AsyncScope                       → TaskGroup
AsyncStatistics                  → ExecutorStats
AsyncFileService                 → FileIO
MainThreadMailbox                → detail::MainThreadMailbox
MainThreadStateCache             → detail 或明确领域 Cache
```

### 2.3 目标接口

```cpp
namespace lux::engine::execution
{
    class Executor final
    {
    public:
        static expected<Executor, OpenError>
        open(ExecutorConfig, MainThreadId);

        template<Operation Op>
        OperationClient<Op> client() const;

        MainThreadScheduler mainThread() const noexcept;
        TaskGroup makeTaskGroup();

        void drainMainThread(std::size_t budget);
        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

不提供：

```cpp
template<class T> T& get();
void* service(std::type_index);
```

### 2.4 Operation 注册

Engine 内置 Operation 在组合根创建 `ExecutorBuilder` 时注册。动态 Extension 使用专用 `ExtensionRegistrar::operations()` 注册未发布 Operation Bundle。

注册流程：

```text
Extension callback
→ create unpublished OperationBundle
→ validate dependencies/queue capacity
→ install on Executor coordinator
→ publish remaining domain descriptors
```

保留当前“先安装异步 Bundle、再提交 Catalog”的安全顺序，但命名与所有者归位。

### 2.5 `EditorAsyncService` 不进入 Engine Execution

Editor Operation 按领域归属：

```text
import              → editor::Content
thumbnail           → editor::Thumbnails
material compile    → toolchain::MaterialCompiler
flow compile        → toolchain::FlowCompiler
scene cook          → toolchain::SceneCooker
model generation    → toolchain::ModelImporter
```

Executor 只执行，不聚合用例。

## 3. Engine AssetStore

### 3.1 合并当前实现

MOVE：

```text
modules/resource/asset/core/AssetManager.*
engine/runtime/assets/AssetLoadService.*
→ engine/assets/AssetStore.*
→ engine/assets/AssetLoading.*
```

公共 `lux::asset::Vfs/CodecRegistry/Provider` 作为构造依赖。

### 3.2 目标所有权

```cpp
class AssetStore final
{
public:
    static expected<AssetStore, OpenError>
    open(
        asset::Vfs&,
        asset::CodecRegistry&,
        execution::Executor&,
        AssetBudget);

    LoadTicket load(asset::AssetId);
    AssetHandle find(asset::AssetId) const noexcept;
    void release(AssetHandle);

    AssetStoreStats stats() const noexcept;
    CloseTask close();

private:
    struct State;
    std::unique_ptr<State> state_;
};
```

### 3.3 Handle

动态缓存对象不得向长生命周期消费者暴露裸指针：

```cpp
struct AssetHandle
{
    AssetSlot slot;
    std::uint32_t generation;
};
```

访问：

```cpp
const T* tryGet<T>(AssetHandle, asset::AssetTypeIdView expected) const noexcept;
```

跨异步任务输入使用不可变值快照或 `AssetLease`，不能捕获 `AssetManager*`。

### 3.4 Scene Asset Adapter

`SceneAssetServices.hpp` 改为明确 Adapter：

```text
engine/scene/integration/assets/SceneAssetResolver.hpp
```

它实现 Scene Feature 需要的窄接口，不将整个 AssetStore 放入 `SystemUpdateContext`。

## 4. Extension API 与 Loader

> `ADR-20260821_ExtensionAbiV4Owner与Core清零.md` 修订本节：ABI v4 的实体定义本轮归
> `engine/extensions/api` 并删除 Core 组件，但 ABI-facing registrar/draft/lease 名称和对象
> 布局不在 v4 内改名。以下 `ExtensionRegistrar/ExtensionDraft/ExtensionLease` 是未来 ABI
> 版本或纯宿主内部重构方向，不得作为 v4 owner 搬迁的验收条件。

### 4.1 不创建 `Modules`

产品组合根拥有：

```cpp
extensions::ExtensionLoader extensions;
```

这只是动态扩展加载与卸载所有者，不是所有服务的容器。

### 4.2 文件迁移

MOVE：

```text
modules/core/extension_abi/*（已删除）
→ engine/extensions/api/*（DONE）

engine/runtime/extensions/loader/ExtensionModuleManager.*
→ engine/extensions/loader/ExtensionLoader.*

engine/runtime/extensions/loader/ModuleLifetime.hpp
→ 后续宿主内部整理；v4 registrar 可见布局暂不改名

engine/runtime/extensions/contribution_host/RuntimeContributionRegistrar.*
→ v4 ABI-facing surface 保留名称；后续新 ABI 再评估改名

EngineExtensions.*
→ 拆除
```

### 4.3 Extension Registrar

一个 Engine Extension 入口可以接收一个专用 Registrar，但 Registrar 只收集领域描述，不提供任意服务查询。

当前 ABI v4 的 canonical 类型仍为 `RuntimeContributionRegistrar` 与
`EditorContributionRegistrar`。下面的 `ExtensionRegistrar` 仅是未来版本草图，不在本轮创建。

```cpp
class ExtensionRegistrar final
{
public:
    ecs::ComponentSchemaRegistrar& components() noexcept;
    scene::FeatureRegistrar& sceneFeatures() noexcept;
    render::ExtensionRegistrar& render() noexcept;
    execution::OperationRegistrar& operations() noexcept;

    ExtensionDraft finish() &&;
};
```

关键区别：

- `ExtensionRegistrar` 位于 Engine Extension SDK，语义明确；
- `SceneFeatureId`、`Render FeatureId`、`ComponentSchemaId` 分别由领域定义；
- 没有通用 `ContributionId`；
- 没有 `get<T>()`；
- 不创建 `engine::module::Modules`。

### 4.4 `EngineExtensions` 拆分

当前 `EngineExtensions` 同时编排 Loader、Catalog、Activation 与 Close。目标拆为：

```text
ExtensionLoader       动态库、descriptor、依赖图、lease
ExtensionDraft        未发布注册结果
ExtensionActivation   产品私有的一次提交过程
各领域 Catalog        Component / Scene Feature / Render / Operation
```

`ExtensionActivation` 可是 `.cpp` 内部函数，不必成为公共类型：

```cpp
expected<LoadedExtension, ActivationError>
activateExtension(
    ExtensionLoader&,
    ExtensionDescriptor,
    CatalogSet&);
```

### 4.5 卸载

卸载前检查：

```text
Extension-owned Operations 无 accepted work
Scene-owned Feature 已关闭
Schedule-owned Systems 已移除
Render-owned Feature/Effect 已关闭
Reflection metadata 无 live object
所有 ExtensionLease 归零
```

满足后才 unload DynamicLibrary。

## 5. Scene

### 5.1 `SceneRuntime → Scene`

MOVE：

```text
engine/runtime/scene/core/include/.../SceneRuntime.hpp
→ engine/scene/core/include/lux/engine/scene/Scene.hpp

SceneRuntime.cpp
→ Scene.cpp

SceneRuntimeCloseSender.hpp
→ CloseTask 或 SceneClose.hpp（内部）
```

目标：

```cpp
class Scene final
{
public:
    static expected<Scene, OpenError>
    open(SceneConfig, SceneDependencies);

    ecs::World& world() noexcept;
    ecs::Schedule& schedule() noexcept;
    Features& features() noexcept;

    void update(const Frame&);
    CloseTask close();

private:
    struct State;
    std::unique_ptr<State> state_;
};
```

`SceneDependencies` 是构造期结构体，只含精确引用：

```cpp
struct SceneDependencies
{
    execution::Executor& executor;
    assets::AssetStore& assets;
    events::Events& events;
    ecs::ComponentTypeCatalog& components;
    FeatureCatalog& features;
};
```

Scene 不保存一个通用 Engine Environment。

### 5.2 Features

当前：

```text
SceneContributionDescriptor
SceneContributionCatalog
SceneContributions facade
SceneContributionHost
BatchBuilder / Bootstrap / InstalledBatch
```

目标业务表面：

```text
FeatureDescriptor
FeatureCatalog
Features
FeatureHandle
```

事务细节放 `detail`：

```text
FeatureTransaction
ServiceBatch
SystemBatch
FeatureLease
```

### 5.3 Scene Feature 安装

```cpp
expected<FeatureHandle, FeatureError>
Features::enable(FeatureIdView id, std::span<const std::byte> config);
```

实现：

```text
lookup descriptor
resolve dependency closure
create services/systems unpublished
validate
commit SceneServices
commit Schedule
publish active feature
```

### 5.4 Scene Script

`SceneScriptRuntime` 当前只是 Scene+Script 的生命周期包装。拆分：

```text
脚本 ECS 行为 → ecs/script
Script backend → modules/function/script
Scene 安装脚本能力 → engine/scene/features/script
会话启动/停止入口 → engine/game/Session
```

删除单独 `SceneScriptRuntime` 类型。

## 6. Render Integration

### 6.1 `RenderBackendHost`

其真正职责是渲染线程、Channel、Server、Session 与 Backend 生命周期。为满足公共 Render SDK，MOVE 到 `modules/function/render` 并收敛为 `render::Renderer`。

Engine 不再拥有第二个 Renderer wrapper。

### 6.2 Scene Bridge

保留 Engine/ECS 到公共 Renderer 的适配：

```text
engine/runtime/render/scene
→ engine/render/scene_bridge
```

职责：

```text
ECS extraction
SceneView 生命周期
Entity/Component → Render Packet
AssetHandle → Render Upload
Scene close 与 View close 协调
```

不得把 `Renderer` 内部 Channel 暴露给 Scene。

### 6.3 `EditorRenderInfra`

删除宽泛裸指针包。调用者接收：

```text
render::Renderer&
render::SceneView&
render::OffscreenView&
assets::AssetStore&
```

中的精确子集。

## 7. Spatial 与内置 Feature

### 7.1 World Partition

MOVE：

```text
resource/spatial3d_scene
engine/runtime/spatial3d/partitioned
engine/runtime/spatial_partition
→ engine/spatial3d/world_partition
```

目录内部再分：

```text
format/        Engine Scene Package 的 feature config
runtime/       interest/residency/streaming
toolchain/     cook and catalog generation（实际代码留 toolchain）
```

### 7.2 Runtime Packs

当前 `engine/runtime/packs` 预装配多个 ECS 域。目标不再称 Pack。

按内容裁决：

```text
单一 Scene Feature 的 descriptor/factory
    → engine/scene/features/<name>

一组默认 Feature 的产品配方
    → engine/game/standard_features

Editor-only Feature
    → engine/editor/features
```

示例：

```text
runtime_pack_physics3d
→ engine_scene_feature_physics3d

runtime_pack_presentation3d
→ engine_scene_feature_presentation3d
```

## 8. Frame 与关闭

### 8.1 FrameCoordinator 内收

`FrameCoordinator` 负责：

```text
begin frame
drain main thread mailbox
dispatch Events
run Scene
submit Renderer
safe-point commits
```

这是产品循环的协作实现，不应安装为通用 Runtime Component。

目标：

```text
engine/game/src/GameFrames.*
engine/editor/src/EditorFrames.*
```

可共享一个 `engine/frame/detail/FramePhases` 内部库，但不公开。

### 8.2 统一 CloseTask

将多个公开：

```text
AsyncRuntimeCloseSender
AsyncScopeCloseSender
SceneRuntimeCloseSender
EngineExtensionsCloseSender
RenderEffectCloseSender
```

收敛为 Engine 内部统一协议：

```cpp
class CloseTask
{
public:
    bool poll();
    bool done() const noexcept;
    std::optional<CloseError> error() const;
};
```

底层可以继续用 sender/receiver；产品 API 只暴露一个 close 结果形状。

### 8.3 产品关闭顺序

```text
1. 关闭用户命令与 Extension load admission
2. 停止 Editor/Toolchain producer
3. 关闭 Play/Preview/Game Session
4. 关闭 Scene Features 与 Schedule
5. 关闭 Scene asset/loading tasks
6. 关闭 Extensions
7. 排空 Executor main-thread completion
8. 关闭 AssetStore
9. 关闭 Renderer
10. 关闭 Executor
11. 释放 catalogs、VFS 与平台对象
```

Renderer 与 Executor 的具体先后要以 accepted-work 依赖验证；任何调整必须有故障注入测试。

## 9. CMake 重构

### 9.1 删除 `engine/runtime/CMakeLists.txt` 安装聚合

在所有子目录迁出后 DELETE：

```text
engine/runtime/
lux-engine-runtime package
lux::engine::runtime namespace targets
```

Engine 内部包可按领域安装，或只作为产品内部目标，不必复制 Modules 的公共 SDK 模式。

### 9.2 新内部目标

```text
lux::engine::execution
lux::engine::assets
lux::engine::extensions_api
lux::engine::extensions_loader
lux::engine::scene
lux::engine::scene_package
lux::engine::scene_loading
lux::engine::render_scene_bridge
lux::engine::spatial3d
```

只有 Extension API 需要单独 SDK 安装；其他是否安装取决于外部 Engine Embed use case，不自动进入公共 Modules SDK。

## 10. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| ENG-01 | 物理目录与新 target 骨架，不改实现 | 所有旧 target alias 可用 |
| EXEC-01 | `AsyncRuntime → Executor` 外观 | 异步测试全通过 |
| ASSET-01 | `AssetStore` 建立并迁移 Manager/LoadService | Resource 无 Manager |
| EXT-01 | Extension API/Loader 路径迁移 | Modules SDK 无 Extension |
| EXT-02 | 拆 `EngineExtensions` 与通用 ContributionId | load/unload 测试 |
| SCENE-01 | `SceneRuntime → Scene` 外观 | 当前 Scene tests |
| SCENE-02 | Contribution 表面压缩为 Feature | 事务/回滚测试 |
| SCENE-03 | Entity Loading 重组 | streaming tests |
| RENDER-ENG-01 | BackendHost 移入 Render，保留 Scene Bridge | 外部 Render sample |
| FRAME-01 | Frame/Close 内收产品 | 无公开 runtime_frame |
| ENG-FINAL | 删除 `engine/runtime` 与旧 package | 全仓无旧 namespace |

## 11. 验收闸门

- [ ] 不存在 `engine/module/Modules.hpp` 或等价通用容器。
- [ ] `ExtensionLoader` 不提供服务定位。
- [ ] `Executor` 仍位于 Engine，公共 Modules 不依赖它。
- [ ] `AssetStore` 位于 Engine，公共 Asset 不拥有缓存。
- [ ] `Scene` 不接收通用 Runtime。
- [ ] `Schedule` 仍唯一拥有 System。
- [ ] `RenderBackendHost` 已由公共 `render::Renderer` 取代。
- [ ] `engine/runtime/packs` 已按 Feature 或产品配方归位。
- [ ] `engine/runtime` 目录和安装包最终删除。
- [ ] 多种 CloseSender 不再成为跨领域公共概念。
- [ ] Extension unload 在所有 Lease 与 accepted work 归零后发生。


---

# Game、Editor 与共享 Session 产品重构

> 删除 Host 产品层与公开 Runtime 容器，让导出游戏无 Engine/Editor 语义，并使 Editor Play 复用同一游戏执行会话

**执行文档 07 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Game、Player、Editor、Platform Entry、Game Exporter 与产品测试负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** Game/Editor 继续装配现有公共 `AssetManager` 与 `engine/runtime/assets`，并在标准 Codec descriptors 后追加 Engine Scene descriptor。本文中的 `AssetStore` 与 `ScenePackage` 未来接口示例已被取代，现行类型为 `AssetManager` 与 `SceneAsset/SceneDescription`。启动清单必须显式指定 boot Scene。


## 1. 核心关系

正确关系不是：

```text
EngineRuntime
├── GameRuntime
└── EditorRuntime
```

而是：

```text
公共 Modules + ECS + Engine 领域实现
                  ↓
        game::Session（共享执行实例）
          ↙                 ↘
game::Game 产品          editor::Editor 产品
```

Editor 包含“创建游戏会话的能力”，而不是继承或嵌入完整 Game 产品。

## 2. 目标目录

```text
engine/
├── game/
│   ├── include/lux/game/
│   │   ├── Game.hpp
│   │   ├── GameConfig.hpp
│   │   └── Session.hpp       # 可设为 private install 或不安装
│   └── src/
├── editor/
│   └── ...
└── game/deployment/
    ├── GameManifest.hpp
    └── GameManifestCodec.cpp

products/
├── player/
│   ├── desktop/main.cpp
│   └── android/...
├── editor/
│   └── main.cpp
└── launcher/
```

最终删除 `engine/hosts` 作为通用层。平台入口是产品源文件，不需要 Host 类。

## 3. `GameApplication → Game`

### 3.1 文件迁移

MOVE：

```text
engine/hosts/game_application/include/.../GameApplication.hpp
→ engine/game/include/lux/game/Game.hpp

engine/hosts/game_application/src/GameApplication.cpp
→ engine/game/src/Game.cpp

engine/hosts/game_application/src/GameApplication.State.inl
→ engine/game/src/GameState.hpp 或 Game.cpp 内部 State
```

DELETE：

```text
engine/hosts/game_application/CMakeLists.txt
lux::engine::host::game_application
```

### 3.2 目标 API

```cpp
namespace lux::game
{
    class Game final
    {
    public:
        static expected<Game, OpenError>
        open(GameConfig, platform::NativeWindowHandle);

        Game(Game&&) noexcept;
        Game& operator=(Game&&) noexcept;
        ~Game();

        expected<void, FrameError>
        frame(const input::InputSnapshot&, math::Extent2u);

        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

公开 Game API 不返回：

```text
Executor&
AssetStore&
Renderer&
ExtensionLoader&
Scene&
```

调试与测试如需观察，使用窄 `GameDiagnostics` Snapshot，而不是暴露内部 owner。

### 3.3 `Game::State`

```cpp
struct Game::State
{
    events::Events events;
    engine::execution::Executor executor;
    asset::Vfs vfs;
    asset::CodecRegistry codecs;
    engine::assets::AssetStore assets;
    render::Renderer renderer;
    engine::extensions::ExtensionLoader extensions;
    input::Input input;

    std::unique_ptr<game::Session> session;
    GameFrames frames;
    GameState state;
};
```

这是 `.cpp` 内私有存储，不是新的架构对象，不提供 `get<T>()`。

## 4. `game::Session`

### 4.1 职责

一个 Session 表达一次游戏执行：

```text
解析 Scene Package
验证 Required Extensions
创建 Asset View
启用标准 Scene Features
创建主 Scene
安装 Script Feature
加载启动 Sections
管理暂停、时间尺度和会话级输入
关闭 Scene 与会话级任务
```

不负责：

```text
Window
Renderer 设备所有权
Executor 线程池所有权
AssetStore 进程缓存所有权
Editor Workspace
```

### 4.2 API

```cpp
namespace lux::game
{
    struct SessionDependencies final
    {
        engine::execution::Executor& executor;
        engine::assets::AssetStore& assets;
        render::Renderer& renderer;
        engine::extensions::ExtensionLoader& extensions;
        events::Events& events;
        input::Input& input;
        engine::scene::FeatureCatalog& sceneFeatures;
    };

    class Session final
    {
    public:
        static expected<Session, OpenError>
        open(
            const engine::scene::package::ScenePackage&,
            SessionDependencies);

        void update(const SessionFrame&);
        engine::scene::Scene& scene() noexcept;
        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

Session 接收精确依赖引用，不接收 `Game&`、`Editor&` 或 Engine Runtime。

## 5. Editor Play 复用 Session

### 5.1 当前正确机制

当前 `EditorScene::enterPlay()` 已经先通过生产 Cook 路径生成独立数据，再启动独立 Scene；这一行为必须保留。

### 5.2 目标流程

```text
SceneDocument
→ save/validate authoring state
→ SceneCooker produces immutable ScenePackage + Section images
→ create play Asset View
→ game::Session::open(...)
→ Editor frame drives Session::update(...)
→ exitPlay closes Session
```

禁止：

```text
直接模拟 Edit registry
复制 GameApplication::Impl 装配
为 Editor 建立另一套 Scene Feature
跳过 production codec
```

### 5.3 Editor State

```cpp
struct Editor::State
{
    events::Events events;
    engine::execution::Executor executor;
    asset::Vfs vfs;
    asset::CodecRegistry codecs;
    engine::assets::AssetStore assets;
    render::Renderer renderer;
    engine::extensions::ExtensionLoader extensions;
    input::Input input;

    Workspace workspace;
    Workbench workbench;

    std::unique_ptr<game::Session> play;
    std::vector<std::unique_ptr<PreviewScene>> previews;

    EditorFrames frames;
};
```

注意：Editor 与 Game 拥有同“种类”的基础能力，但不要求共享同一个顶层对象或继承关系。

## 6. 删除 `GameHost`

### 6.1 当前文件

```text
engine/hosts/player/include/.../GameHost.hpp
engine/hosts/player/src/GameHost.cpp
engine/hosts/player/app/main.cpp
```

### 6.2 目标

`products/player/desktop/main.cpp` 直接装配 Window 与 Game：

```cpp
int main(int argc, char** argv)
{
    auto manifest = game::deployment::GameManifest::load(...);
    if (!manifest)
        return report(manifest.error());

    auto window = platform::glfw::Window::open(
        {.title = manifest->title});
    if (!window)
        return report(window.error());

    auto game = game::Game::open(
        makeGameConfig(*manifest),
        window->nativeHandle());
    if (!game)
        return report(game.error());

    while (!window->closeRequested())
    {
        window->pollEvents();
        auto input = platform::glfw::captureInput(*window);
        if (auto frame = game->frame(input, window->extent()); !frame)
            return report(frame.error());
    }

    return finish(game->close());
}
```

平台入口可有小型局部 helper，但不再创建 `GameHost` 类。

## 7. Editor 产品入口

MOVE：

```text
engine/hosts/editor
→ products/editor
```

产品入口只负责：

```text
命令行解析
Window 创建
Editor::open
平台事件循环
顶层错误报告
```

Editor 实现位于 `engine/editor`，产品入口不拥有业务 Controller。

## 8. Game Manifest 与导出语义

### 8.1 文件迁移

MOVE：

```text
modules/resource/deployment/RuntimeLaunchManifest.*
→ engine/game/deployment/GameManifest.*
```

### 8.2 字段

RENAME：

```text
game_pak    → gamePack
engine_pak  → basePack
boot_scene  → bootPackage
extensions  → requiredExtensions
capacity    → productOptions/renderOptions（按领域拆分）
```

目标：

```cpp
struct GameManifest final
{
    static constexpr std::uint32_t schemaVersion = 5;

    std::string title;
    std::filesystem::path basePack;
    std::filesystem::path gamePack;
    std::string bootPackage;
    GameOptions game;
    render::RenderConfig render;
    std::vector<RequiredExtension> requiredExtensions;
};
```

### 8.3 导出目录

导出程序中避免 Engine/Editor 语义：

```text
MyGame/
├── MyGame.exe
├── game.toml
├── content/
│   ├── base.pak
│   └── game.pak
├── extensions/
└── libraries/
```

不输出：

```text
engine.pak
engine_runtime.dll
editor_*
EngineRuntime
```

内部动态库文件名若暂时含 `lux_engine_*`，可作为后续 ABI/发布迁移；用户可见 Manifest 与 API 先清理。

## 9. Game 与 Editor 的基础能力复用方式

### 9.1 共享 Builder 函数，而非共享 Runtime 容器

建立私有创建函数：

```cpp
expected<execution::Executor, Error>
openExecutor(const ExecutionConfig&);

expected<assets::AssetStore, Error>
openAssets(asset::Vfs&, asset::CodecRegistry&, execution::Executor&);

expected<render::Renderer, Error>
openRenderer(const render::RenderConfig&, platform::NativeWindowHandle);

expected<extensions::ExtensionLoader, Error>
openExtensions(...);
```

Game 与 Editor 调用同一函数，但各自显式拥有结果。

### 9.2 领域 Bundle 仅允许私有返回值

若构造错误回滚需要一次返回多个对象，可使用 `.cpp` 私有：

```cpp
struct BaseFacilities
{
    Events events;
    Executor executor;
    Vfs vfs;
    CodecRegistry codecs;
    AssetStore assets;
    Renderer renderer;
    ExtensionLoader extensions;
};
```

禁止安装头文件、禁止 `get<T>()`、禁止业务对象保存 `BaseFacilities&`。

## 10. Frame 循环

### 10.1 Game

```text
poll input（产品 main）
→ Game::frame
  → drain executor main-thread completion
  → dispatch events
  → update session/scene
  → render frame
  → commit safe-point mutations
```

### 10.2 Editor

```text
poll input（产品 main）
→ Editor::frame
  → drain executor
  → update filewatch/content
  → update active documents
  → update optional play session
  → update previews
  → draw workbench
  → render
  → commit safe points
```

共享顺序可由私有 `FramePhases` helper 表达，不能再次建立公共 `FrameCoordinator` 服务。

## 11. 关闭协议

### 11.1 Game

```text
close product admission
→ close Session
→ close Extensions
→ close AssetStore
→ close Renderer
→ close Executor
→ release Window in product main
```

### 11.2 Editor

```text
stop import/compile/filewatch producers
→ close Workbench connections
→ close play Session
→ close PreviewScenes
→ close Documents/Workspace
→ close Extensions
→ close AssetStore
→ close Renderer
→ close Executor
```

析构只做同步兜底或断言已关闭；复杂异步关闭必须由 `close()` 完成。

## 12. CMake 迁移

### 12.1 新目标

```text
lux::game
lux::editor
lux_player
lux_editor
```

`lux::game` 是 Engine 产品库，可供 Editor 内部链接；它不等于 `lux_player` 可执行程序。

### 12.2 删除

```text
lux::engine::host::game_application
lux::engine::host::player
engine/hosts/game_application
engine/hosts/player
```

### 12.3 依赖

`lux::game` 不依赖：

```text
GLFW
ImGui
Editor
Authoring
Toolchain importer
```

`lux_player` 依赖：

```text
lux::game
lux::window_glfw
```

`lux::editor` 可以依赖 `lux::game` 的 Session 实现，但不依赖 `lux_player`。

## 13. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| PRODUCT-01 | 新 `Game` façade 包住 GameApplication | 行为不变 |
| SESSION-01 | 提取 `game::Session` | Player 仍通过旧 façade |
| SESSION-02 | Editor Play 改用 Session | Play/Cook 一致性 |
| PRODUCT-02 | Product main 直接驱动 Game | GameHost 无消费者 |
| PRODUCT-03 | 删除 GameHost 与 game_application target | Desktop/Android 测试 |
| MANIFEST-01 | GameManifest 新路径和字段外观 | 旧 manifest 可读 |
| EXPORT-01 | 导出目录去 Engine/Editor 用户语义 | inventory 测试 |
| PRODUCT-04 | `LuxEditor → Editor` façade | Editor 行为不变 |
| PRODUCT-FINAL | 删除 engine/hosts | 所有产品入口位于 products |

## 14. 验收闸门

- [ ] 没有公开 `EngineRuntime`、`GameRuntime`、`EditorRuntime`。
- [ ] `Game` 与 `Editor` 是独立 owner。
- [ ] Editor Play 与 Player 使用同一个 `game::Session::open`。
- [ ] `GameHost` 已删除。
- [ ] `GameApplication` 已删除或仅作为短期 deprecated façade。
- [ ] `lux::game` 不依赖 GLFW/ImGui/Editor。
- [ ] `lux::editor` 不依赖 Player executable。
- [ ] 导出 Manifest 不包含 `engine_pak`。
- [ ] 导出目录无 Editor/Toolchain 二进制。
- [ ] Product API 不公开内部 Executor/AssetStore/Renderer。
- [ ] Close 失败可诊断，不以 `Impl::release()` 泄漏整个产品状态作为常规路径。


---

# Editor：Workspace、Workbench、Documents 与 Panels 重构

> 拆除 LuxEditor 上帝对象、Controller 网络、异步总服务与 Panel hook，使全部编辑功能遵循同一文档/命令/视图模型

**执行文档 08 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Editor、Authoring、Toolchain、UI、Preview、Flow、Material、Script 与 Content 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** Editor 不创建独立 AssetStore。临时场景也构造并注册 `SceneAsset`，复用公共 `AssetManager`、Catalog 与异步加载适配器；本文其它 Workspace/Workbench 目标不因本次 Scene Asset 施工提前推进。


## 1. 目标结构

```text
Editor
├── Workspace
│   ├── Project
│   ├── Content
│   ├── ContentIndex
│   ├── ContentWatcher
│   └── Documents
│       ├── SceneDocument
│       ├── MaterialDocument
│       ├── FlowDocument
│       └── ScriptDocument
├── Workbench
│   ├── UI
│   ├── MainMenu
│   ├── PanelCatalog
│   └── active Panels
├── optional<game::Session> play
└── PreviewScene[]
```

| 当前类型 | 目标 | 处理 |
| --- | --- | --- |
| `LuxEditor` | `lux::editor::Editor` | 产品 façade；私有 State |
| `LuxEditor::Runtime` | 删除 | 字段分配到 Editor::State、Workspace、Workbench、Preview |
| `EditorShell` | `Workbench` | UI 与 Panel composition |
| `EditorToolHost` | `Workbench::Panels` 或合入 Workbench | 删除 Host |
| `EditorTools` façade | 删除 | Workbench 提供窄 Panel API |
| `ProjectController` | `Workspace` | 工程与内容所有者 |
| `SceneController` | `Documents` / `SceneDocument` | 文档生命周期 |
| `ImportController` | `Content::import` | ImportDialog 只处理 UI |
| `AssetDeleteController` | `Content::remove` | 删除确认与业务分离 |
| `AssetRegistry` | `ContentIndex` | Authoring 文件索引 |
| `AssetFileWatcher` | `ContentWatcher` | 文件事件→Content 变更 |
| `AssetBrowser` | `ContentBrowser` | View |
| `EditorAsyncService` | 删除 | Operation 按领域归位 |
| `FlowForgeCompilerService` | `toolchain::FlowCompiler` | Panel 不直接拥有 |
| `MaterialGraphPanel` | `MaterialEditor` | Document+Commands View |
| `FlowGraphPanel` | `FlowEditor` | Document+Commands View |
| `LuaConsole` | `ScriptEditor` 或 `ScriptConsole` | 不承担 Script Runtime ownership |
| `SceneFeatureSettingPanel` | `SceneSettings` | View |
| `MaterialPreviewHost` | `MaterialPreview` | 基于 PreviewScene |
| `ThumbnailService` | `Thumbnails` | 基于 PreviewScene |
| `EditorRenderInfra` | 删除 | 精确 Renderer/View/Asset dependencies |

## 2. `LuxEditor::Runtime` 解体

### 2.1 当前问题

当前 `LuxEditor::Runtime` 同时拥有：

```text
事件、渲染、资产、异步、扩展
项目、文件监视、场景
UI、所有 Panels、所有 Controllers
Flow/Material compiler
Preview/Thumbnail
hooks、subscriptions、pending actions
```

并依赖字段声明顺序形成隐式销毁图。

### 2.2 迁移方法

先建立 `Editor::State`，但明确它只是 `.cpp` 私有存储：

```cpp
struct Editor::State
{
    // process/product facilities
    Events events;
    Executor executor;
    AssetStore assets;
    Renderer renderer;
    ExtensionLoader extensions;
    Input input;

    // editor domains
    Workspace workspace;
    Workbench workbench;
    PreviewPool previews;

    std::unique_ptr<game::Session> play;
    EditorFrames frames;
};
```

禁止：

```cpp
state.get<T>();
panel(Editor::State&);
controller(Editor&);
```

### 2.3 字段迁移规则

| 字段类型 | 目标 owner |
| --- | --- |
| Project、AssetRegistry、FileWatcher | Workspace |
| Panels、Menu、UI、ToolHost | Workbench |
| Flow/Material compiler clients | Toolchain compiler owners/Workspace |
| Thumbnail、Material Preview | PreviewPool |
| Scene Edit state | SceneDocument |
| Play Scene | `game::Session` |
| Renderer/Executor/AssetStore | Editor::State |
| subscriptions | 最接近订阅者的 owner |

## 3. Workspace

### 3.1 API

```cpp
class Workspace final
{
public:
    static expected<Workspace, OpenError>
    open(
        ProjectPath,
        WorkspaceDependencies);

    Project& project() noexcept;
    Content& content() noexcept;
    ContentIndex& index() noexcept;
    Documents& documents() noexcept;

    expected<void, SaveError> saveAll();
    CloseTask close();

private:
    Project project_;
    ContentIndex index_;
    Content content_;
    ContentWatcher watcher_;
    Documents documents_;
};
```

Workspace 不 include：

```text
Panel.hpp
ImGui
ViewportPanel
UISystem/UI
EditorMenuBar
```

### 3.2 `ProjectController`

MOVE 领域状态和操作到 Workspace：

```text
openProject
closeProject
saveProject
currentProject
cache directory
content root
module requirements
```

Project switching UI 留在 Workbench：

```text
File menu
confirmation modal
file dialog
```

Workbench 调用 `Workspace::open` 或产品层 replacement，不让 Workspace 反向调用 UI。

## 4. Content 与 ContentIndex

### 4.1 `AssetRegistry → ContentIndex`

ContentIndex 只负责 Authoring 磁盘索引：

```cpp
struct ContentEntry
{
    asset::AssetId id;
    asset::AssetTypeId type;
    std::filesystem::path sourcePath;
    VirtualPath virtualPath;
    std::string displayName;
};

class ContentIndex
{
public:
    expected<void, ScanError> rebuild();
    const ContentEntry* find(asset::AssetId) const noexcept;
    std::span<const ContentEntry> children(VirtualPathView) const;
};
```

它不拥有 Runtime Decoded Assets。

### 4.2 `Content`

集中命令：

```cpp
class Content final
{
public:
    ImportTicket import(ImportRequest);
    expected<asset::AssetId, CreateError> create(CreateRequest);
    expected<void, RemoveError> remove(asset::AssetId);
    expected<void, MoveError> move(asset::AssetId, VirtualPathView);
    expected<void, RenameError> rename(asset::AssetId, std::string_view);
};
```

迁入当前散布在：

```text
EditorShell::wireAssetServices
ImportController
AssetDeleteController
AssetBrowser create/delete hooks
```

中的文件操作与事件发布。

### 4.3 事件

命令完成后 Content 发布事实：

```text
ContentImported
ContentCreated
ContentRemoved
ContentMoved
ContentRenamed
```

事件不承载请求，也不返回结果。

## 5. Documents

### 5.1 通用合同

```cpp
class Document
{
public:
    virtual ~Document() = default;
    virtual DocumentId id() const noexcept = 0;
    virtual asset::AssetId asset() const noexcept = 0;
    virtual bool dirty() const noexcept = 0;
    virtual expected<void, SaveError> save() = 0;
};

class Documents final
{
public:
    OpenTicket open(asset::AssetId);
    Document* find(DocumentId) noexcept;
    expected<void, CloseError> close(DocumentId, CloseMode);
};
```

避免建立万能虚基类后让所有编辑器依赖大量虚函数；可用 variant/typed handles。核心要求是统一文档生命周期和 dirty/save/close 语义。

### 5.2 文档与 View 分离

每个领域遵循：

```text
Document       唯一可编辑状态
Commands       修改 Document 的用例
Compiler       消费不可变 Snapshot
Editor Panel   绘制与用户输入
Codec/Repository 读取写入
```

Panel 不拥有 AssetManager、Events、Executor、Project 路径。

## 6. Workbench

### 6.1 文件迁移

```text
EditorShell.*        → Workbench.*
EditorMenuBar.*      → Workbench/MainMenu.*
EditorToolHost.*     → Workbench/Panels.*
EditorTools.*        → 删除
EditorPanelCatalog.* → PanelCatalog.*
```

### 6.2 API

```cpp
class Workbench final
{
public:
    Workbench(UI&, Workspace&, PanelCatalog&);

    PanelHandle openPanel(PanelIdView);
    void closePanel(PanelHandle);
    Panel* activePanel() noexcept;

    void frame();

private:
    UI& ui_;
    Workspace& workspace_;
    PanelCatalog& catalog_;
    PanelSet panels_;
    MainMenu menu_;
};
```

Workbench 可以知道 Workspace 的用例 API，但不进行 Asset Codec、Scene Cook 或编译。

### 6.3 Panel Factory Context

允许在唯一装配边界使用类型擦除：

```cpp
descriptor.create = [](const PanelContext& context)
{
    auto& documents = context.require<Documents>();
    auto& commands = context.require<FlowCommands>();
    return std::make_unique<FlowEditor>(documents, commands);
};
```

限制：

- Context 只在 Factory 调用期间存在；
- Panel 构造后不得保存 Context；
- Descriptor 明确列出 required types；
- 缺失依赖返回结构化错误；
- 普通业务代码不得调用 `context.find<T>()`。

## 7. 消除 Hook 与两阶段构造

### 7.1 禁止接口

```text
setAssetServices
setPreviewHost
setCompileDispatch
setPrecompileHook
setCreateMenuHook
setDeleteAssetHandler
setActivateHandler
setSceneSettingsAccessor
setAvailableComponentsProvider
```

### 7.2 替代方式

| 当前 hook | 替代 |
| --- | --- |
| Save/Create/Delete | `Content` 或 Document Command 方法 |
| Compile | Compiler Client |
| Panel activation | Workbench 直接方法 |
| 已提交事实 | Events |
| Extension panel | Panel Descriptor |
| Preview update | MaterialCommands/Preview API |
| Scene settings access | `SceneDocument&` |

必需依赖构造注入；可选当前 Document 使用 `DocumentHandle` 或 `optional<reference_wrapper<T>>`，而不是后续 setter。

## 8. Flow 全链路

### 8.1 目标文件

```text
engine/authoring/flowforge/
    FlowDocument.hpp/.cpp
    FlowCodec.hpp/.cpp

engine/toolchain/flowforge/
    FlowCompiler.hpp/.cpp
    FlowCompileRequest.hpp

engine/editor/src/flow/
    FlowCommands.hpp/.cpp
    FlowEditor.hpp/.cpp
    FlowGraphView.hpp/.cpp
    FlowSchema.hpp/.cpp
```

### 8.2 `FlowDocument`

```cpp
class FlowDocument final
{
public:
    flowforge::FlowGraph& graph() noexcept;
    const flowforge::FlowGraph& graph() const noexcept;

    bool dirty() const noexcept;
    FlowSnapshot snapshot() const;

private:
    flowforge::FlowGraph graph_;
    DirtyState dirty_;
};
```

### 8.3 Compiler

```cpp
class FlowCompiler final
{
public:
    CompileTicket compile(FlowCompileRequest);

private:
    execution::OperationClient<CompileFlowOperation> client_;
};
```

请求拥有完整快照和输出路径；后台线程不访问 Panel、Workspace 当前状态或 AssetManager。

### 8.4 Editor

```cpp
class FlowEditor final : public ui::Panel
{
public:
    FlowEditor(
        FlowDocument&,
        FlowCommands&,
        const FlowNodeCatalog&);

private:
    FlowDocument& document_;
    FlowCommands& commands_;
    const FlowNodeCatalog& nodes_;
};
```

DELETE：

```text
FlowGraphPanel::setAssetServices
FlowGraphPanel::setPrecompileHook
NodeRegistry::global()
FlowForgeCompilerService::setPrecompileDispatch
```

`NodeRegistry::global()` 改为 Workspace/Extension 注册形成的 `FlowNodeCatalog`。

## 9. Material 全链路

### 9.1 当前问题

`MaterialGraphPanel` 同时拥有：

```text
graph SSOT
compile job/outcome
Asset save/open
Material instance chain
Preview
Texture picker
Compile dispatch
GLSL/SPIR-V
```

### 9.2 目标文件

```text
engine/authoring/material/
    MaterialDocument.hpp/.cpp
    MaterialInstanceDocument.hpp/.cpp
    MaterialCodec.hpp/.cpp

engine/toolchain/material/
    MaterialCompiler.hpp/.cpp
    MaterialCompileRequest.hpp
    CompiledMaterial.hpp

engine/editor/src/material/
    MaterialCommands.hpp/.cpp
    MaterialEditor.hpp/.cpp
    MaterialGraphView.hpp/.cpp
    MaterialSchema.hpp/.cpp
    MaterialPreview.hpp/.cpp
```

### 9.3 Snapshot 与 latest-wins

保留当前后台编译和 latest-wins 思路，但移出 Panel：

```cpp
struct MaterialCompileRequest
{
    DocumentRevision revision;
    MaterialGraphSnapshot graph;
    TextureBindings textures;
};

struct CompiledMaterial
{
    DocumentRevision revision;
    std::string glsl;
    SpirvBundle spirv;
};
```

`MaterialCommands` 保存 pending revision，过期结果丢弃。

### 9.4 Instance

Material 与 Material Instance 是两个 Document 类型；可以共享一个 Editor View，但不在一个 Panel 中堆积所有状态机。

推荐：

```text
MaterialEditor
  variant<MaterialDocument*, MaterialInstanceDocument*>
```

每个 Document 自己负责父链解析与 dirty state；Preview 只接收 EffectiveMaterial Snapshot。

## 10. Script

`LuaConsole` 若主要用于交互式调用，命名为 `ScriptConsole`；若用于编辑文件，建立 `ScriptEditor`。

Panel 不拥有 `ScriptRuntime`；通过 `ScriptCommands`：

```cpp
class ScriptCommands
{
public:
    RunTicket runSelection(ScriptDocument&, TextRange);
    expected<void, SaveError> save(ScriptDocument&);
};
```

Game/Play Session 的 Script Runtime 与 Editor Console 的 Tooling Runtime 必须是不同实例或明确不同 scope，不能共享隐式全局 Registry。

## 11. SceneDocument

### 11.1 拆 `EditorScene`

当前 `EditorScene` 包含 Edit World、交互、Play、Cook、渲染与生命周期。目标拆为：

```text
SceneDocument          Authoring state、selection、undo、save
SceneCommands          create/delete/reparent/component edit
SceneCooker            Toolchain
SceneViewport          View/interaction
game::Session          Play
```

`SceneDocument` 不拥有 Runtime Scene。

### 11.2 Enter Play

Workbench 用户动作：

```text
SceneCommands::requestPlay
→ Workspace saves/validates
→ SceneCooker
→ Editor creates game::Session
→ SceneViewport retargets to play SceneView
```

退出 Play 后 retarget 回 Edit Preview，不通过 setter 修改大量裸指针；使用明确 `ViewportSource` variant/handle。

## 12. Preview 与 Thumbnail

### 12.1 `PreviewScene`

CREATE：

```cpp
class PreviewScene final
{
public:
    static expected<PreviewScene, OpenError>
    open(
        PreviewTemplate,
        Renderer&,
        Executor&,
        AssetStore&,
        SceneFeatureCatalog&);

    Scene& scene() noexcept;
    render::OffscreenView& view() noexcept;
    CloseTask close();
};
```

### 12.2 迁移

```text
MaterialPreviewHost → MaterialPreview
ThumbnailService RuntimeHost → PreviewScene
PreviewWorldCommon → PreviewTemplate/PreviewScene builder
```

MaterialPreview 与 Thumbnails 共享创建、关闭、View 与 Scene Feature 装配，不再各自实现私有 SceneRuntime Host。

### 12.3 生命周期

统一：

```text
open
update/request
close
```

删除同一对象上的：

```text
initialize
releaseGpu
shutdown
```

## 13. 裸指针处理

### 13.1 改为引用

当前构造参数为引用且对象不允许为空：

```text
FlowGraphView::graph_
FlowGraphView::registry_
FlowSchema::graph_
FlowSchema::registry_
FlowSchema::view_
MaterialGraphView::graph_
MaterialGraphSchema::graph_
MaterialGraphSchema::view_
```

改为：

```cpp
FlowGraph& graph_;
NodeCatalog& nodes_;
```

### 13.2 保留可选指针

瞬时查询：

```cpp
Document* activeDocument() noexcept;
Panel* activePanel() noexcept;
```

可以返回指针；调用者不得跨异步任务保存。

### 13.3 动态对象

Panel/Document/Extension 使用 generation handle，不以裸指针跨帧缓存。

## 14. CMake 与文件迁移

MODIFY `engine/editor/CMakeLists.txt`：

- 删除 `EditorAsyncService.cpp`；
- 删除 Controller 源；
- 新增 Workspace/Content/Documents；
- 新增 Flow/Material/Script 领域目录；
- PreviewScene 公共实现单独源组；
- 不再通过 `src` 私有 include root 随意跨目录 include；
- 对每个内部领域建立明确 private target 或 source group。

推荐内部目标：

```text
lux_editor_workspace
lux_editor_workbench
lux_editor_documents
lux_editor_flow
lux_editor_material
lux_editor_preview
lux_editor_product
```

它们是 Editor 内部目标，不安装为 Modules SDK。

## 15. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| EDIT-01 | `Editor` façade 与 State | 行为不变 |
| EDIT-02 | ContentIndex/Content 建立 | create/import/delete 测试 |
| EDIT-03 | Workspace 替代 ProjectController | 项目切换测试 |
| EDIT-04 | Documents 与 SceneDocument | dirty/save/close 测试 |
| EDIT-05 | Workbench 替代 Shell/ToolHost | Panel 生命周期测试 |
| EDIT-06 | 删除 EditorAsyncService，Operation 归领域 | executor close 测试 |
| FLOW-01 | FlowDocument/Commands/Compiler/Editor | 删除 hooks |
| MAT-01 | MaterialDocument/Compiler/Editor | compile/preview/save 测试 |
| SCRIPT-01 | ScriptDocument/Commands/Console | Runtime scope 测试 |
| PREVIEW-01 | PreviewScene 统一 | thumbnail/material preview |
| EDIT-FINAL | 删除旧 Controllers、Shell、Runtime、hook API | 架构扫描归零 |

## 16. 验收闸门

- [ ] `LuxEditor::Runtime` 已删除。
- [ ] Panel 不持有 `Editor&`、通用 Runtime 或通用 Service Context。
- [ ] 必需依赖无 `set*Service()`/`set*Hook()`。
- [ ] `EditorAsyncService` 已删除。
- [ ] Workspace 不 include UI。
- [ ] Workbench 不执行文件 Codec/编译。
- [ ] Document 是可编辑状态唯一来源。
- [ ] Compiler 只消费 owning Snapshot。
- [ ] `NodeRegistry::global()` 已删除。
- [ ] Material Preview 与 Thumbnail 共用 PreviewScene。
- [ ] `EditorScene` 已拆分，Play 使用 `game::Session`。
- [ ] 长生命周期异步闭包不捕获 Panel/Controller 裸指针。


---

# CMake、命名空间、SDK 包与兼容迁移

> 把仓库内部层级从外部 API 中移除，重建安装包、Target、Include Prefix、依赖分类与自动架构检查

**执行文档 09 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | CMake、发布、SDK、持续集成、ABI 与所有模块负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** 本轮不创建 `lux/asset/AssetId.hpp`、新 `AssetTypeId` 或其兼容 shim。Scene 安装面收敛为单一 `lux-engine-scene` component `scene`；旧 `scene_api`、`scene_package` target/include/component 一次删除。全局 M7 package rename 仍不推进。

> **2026-08-21 裁决更新：** 本轮仅在现有 `lux::engine::resource::asset` target 内迁移领域头路径，不推进全局 M7。新增 `lux::engine::content` / `lux-engine-content` `content` component；旧 Asset `codecs/`、`pak/`、根部领域头与 Resource `BuiltinAssetIds.hpp` 不保留兼容层。

> **2026-08-21 Profile 修订：** `LUX_BUILD_PROFILE` 仍只有 DEVELOPER/PLAYER/EDITOR/TOOLCHAIN；不创建文中历史 `MODULES_SDK` Profile。Modules 包边界改由 installed consumers 验证，旧 alias/forwarding 迁移要求由“一次迁移并删除旧 API”的现行规则取代。

> **2026-08-21 Serialization 安装事实：** `lux-engine-core COMPONENTS serialization` 的导出闭包只有 binary/stduuid；`lux-engine-ecs COMPONENTS component_archive` 独立导出且不含 Resource/Engine。旧 Core TaggedPropertyArchive 头不再安装。

> **2026-08-21 Extension ABI 安装事实：** `lux-engine-extensions COMPONENTS extension_api` 可由 installed consumer 独立配置/链接；`lux-engine-core COMPONENTS extension_abi` 明确失败。旧 Core 头/export 已精确清理，不提供 alias 或 forwarding header。


## 1. 目标

外部用户应按领域消费：

```cmake
find_package(lux-render CONFIG REQUIRED)
target_link_libraries(app PRIVATE lux::render lux::render_vulkan)
```

而不是理解仓库层级：

```cmake
find_package(lux-engine-function COMPONENTS render_client render_vulkan)
target_link_libraries(app PRIVATE
    lux::engine::function::render_vulkan)
```

内部层级分类仍可存在，但不进入用户 API。

## 2. Target 映射

| 旧目标 | 新目标 | 分发 |
| --- | --- | --- |
| `lux::engine::core::events` | `lux::events` | `lux-core` |
| `lux::engine::core::log` | `lux::log` | `lux-core` |
| `lux::engine::core::math` | `lux::math` | `lux-core` |
| `lux::engine::core::serialization` | `lux::serialization` | `lux-core` |
| `lux::engine::ecs::component_archive` | `lux::ecs::serialization` | `lux-engine-ecs` |
| `lux::engine::core::meta` | 删除；使用 `lux::cxx::reflection_runtime`/ECS adapter | 不再独立公共包 |
| `lux::engine::core::extension_abi` | 当前 `lux::engine::extensions::extension_api`；M7 再评估短 alias | `lux-engine-extensions`，Core 旧 component 删除 |
| `lux::engine::platform::common` | 删除 | — |
| `lux::engine::platform::window` | `lux::window` / `lux::window_glfw` | `lux-platform` |
| `lux::engine::platform::gapi` | KEEP；公共低层 Vulkan wrapper SDK | `lux-engine-platform` |
| `lux::engine::platform::dynamic_library` | `lux::dynamic_library` | `lux-platform` |
| `lux::engine::platform::filewatch` | `lux::filewatch` | `lux-platform` |
| `lux::engine::resource::description` | `lux::description` | `lux-resource` |
| `lux::engine::resource::asset_identity` | 合入 `lux::asset` | `lux-resource` |
| `lux::engine::resource::asset_core` | `lux::asset` + `lux::engine::assets` | 拆分 |
| `lux::engine::resource::asset_codecs` | `lux::asset` 的 standard codec registration | `lux-resource` |
| `lux::engine::resource::asset_pak` | `lux::asset_pak` | `lux-resource` |
| `lux::engine::resource::deployment` | 删除；分到 Render Config 与 Game Deployment | — |
| `lux::engine::resource::entity_scene` | `lux::ecs::scene_format` + `lux::engine::scene_package` | 拆分 |
| `lux::engine::resource::spatial` | 合入 `lux::math` | `lux-core` |
| `lux::engine::function::render_client` | `lux::render` | `lux-render` |
| `lux::engine::function::render_graph` | `lux::render_graph` | `lux-render` |
| `lux::engine::function::render_vulkan` | `lux::render_vulkan` | `lux-render` |
| `lux::engine::function::render_features` | `lux::render_standard` | `lux-render` |
| `lux::engine::function::input` | `lux::input` | `lux-input` |
| `lux::engine::function::animation` | `lux::animation` | `lux-animation` |
| `lux::engine::function::navigation` | `lux::navigation` | `lux-navigation` |
| `lux::engine::function::script_core` | `lux::script` | `lux-script` |
| `lux::engine::function::script_lua` | `lux::script_lua` | `lux-script` |
| `lux::engine::function::script_native` | `lux::script_native` | `lux-script` |
| `lux::engine::function::ui` | `lux::ui` / `lux::ui_imgui` | `lux-ui` |
| `lux::engine::function::ui_vulkan` | `lux::ui_render_vulkan` | `lux-ui`/`lux-render` integration |
| `lux::engine::runtime::runtime_execution` | `lux::engine::execution` | Engine internal |
| `lux::engine::runtime::runtime_assets` | `lux::engine::assets` | Engine internal |
| `lux::engine::runtime::runtime_scene_core` | `lux::engine::scene` | Engine internal |
| `lux::engine::runtime::runtime_extension_loader` | `lux::engine::extensions_loader` | Engine internal |
| `lux::engine::runtime::runtime_render_backend_host` | 删除；`lux::render` | `lux-render` |
| `lux::engine::host::game_application` | `lux::game` | Engine product library |

## 3. Include Prefix 迁移

### 3.1 规则

公共 modules：

```text
lux/<domain>/...
```

ECS：

```text
lux/ecs/...
```

Engine：

```text
lux/engine/<domain>/...
```

产品：

```text
lux/game/...
lux/editor/...
```

### 3.2 示例

```text
lux/engine/resource/asset/AssetId.hpp
→ lux/asset/AssetId.hpp

lux/engine/description/Mesh.hpp
→ lux/description/Mesh.hpp

lux/engine/function/render/client/RenderTypes.hpp
→ lux/render/Types.hpp

lux/engine/window/LuxWindow.hpp
→ lux/platform/Window.hpp 或 lux/window/Window.hpp

lux/engine/runtime/scene/SceneRuntime.hpp
→ lux/engine/scene/Scene.hpp
```

### 3.3 Forwarding Header

生成而不是手写大量转发头。CREATE：

```text
cmake/Compatibility/GenerateForwardingHeaders.cmake
```

输入映射：

```cmake
lux_add_forwarding_header(
    OLD lux/engine/resource/asset/AssetId.hpp
    NEW lux/asset/AssetId.hpp
    REMOVE_AFTER 2.0)
```

生成内容只允许：

```cpp
#pragma once
#if defined(_MSC_VER)
#pragma message("deprecated: include <lux/asset/AssetId.hpp>")
#endif
#include <lux/asset/AssetId.hpp>
```

## 4. Package 设计

### 4.1 Modules SDK

```text
lux-core
lux-platform
lux-resource
lux-render
lux-input
lux-animation
lux-navigation
lux-script
lux-ui
```

### 4.2 Engine SDK

按实际外部嵌入需求安装：

```text
lux-engine-ecs
lux-engine-scene
lux-engine-game
lux-engine-extension-sdk
```

Editor、Toolchain 默认是产品/工具，不自动作为通用 SDK。

### 4.3 CPack 组件

```text
lux_modules_sdk
lux_ecs_sdk
lux_engine_sdk
lux_extension_sdk
lux_player
lux_editor
lux_toolchain
```

`lux_modules_sdk` 不包含：

```text
Scene
Extension ABI
Game
Editor
ECS
```

## 5. `install_components` 重构

当前每个层建立一个 `lux-engine-*` Package 并枚举大量细碎 component。目标改为领域包。

建议新增通用函数：

```cmake
lux_install_package(
    PACKAGE lux-render
    NAMESPACE lux::
    TARGETS
        lux_render
        lux_render_graph
        lux_render_vulkan
        lux_render_standard
)
```

函数必须：

- 生成 relocatable Config；
- 不写入 build-tree absolute path；
- 递归收集外部依赖；
- 区分 Build Tool 与 Runtime Dependency；
- 支持 COMPONENT；
- 生成 version file；
- 运行 installed-package smoke test。

## 6. Alias 迁移

### 6.1 原则

真实目标使用新名称：

```cmake
add_library(lux_render ...)
add_library(lux::render ALIAS lux_render)
```

旧目标只能：

```cmake
add_library(lux::engine::function::render_client ALIAS lux_render)
```

不能反过来让新目标 alias 旧目标，否则安装导出仍以旧语义为中心。

### 6.2 Imported Compatibility Package

旧 `find_package(lux-engine-function)` 可在一个版本内生成 compatibility config，它内部查找 `lux-render/lux-input/...` 并创建 deprecated imported aliases。

删除时间写入：

```text
doc/migration/legacy-package-removal.md
```

## 7. Target 分类 DAG

### 7.1 目标层

建议分类枚举：

```text
MODULE_CORE
MODULE_PLATFORM
MODULE_RESOURCE
MODULE_FUNCTION
ECS
ENGINE
AUTHORING
TOOLCHAIN
EDITOR
PRODUCT
TEST
BUILD_TOOL
```

### 7.2 合法边

```text
MODULE_* → 更低 MODULE_*
ECS → MODULE_*
ENGINE → ECS + MODULE_*
AUTHORING → MODULE_* + 可选 ECS schema
TOOLCHAIN → AUTHORING + 格式所有者
EDITOR → ENGINE + AUTHORING + TOOLCHAIN
PRODUCT → 对应产品库 + Platform backend
BUILD_TOOL → 可依赖需要解析的 SDK，但不得进入 Runtime closure
```

### 7.3 禁止边

```text
MODULE_* → ECS/ENGINE/EDITOR/TOOLCHAIN
ECS → ENGINE/EDITOR
ENGINE core → EDITOR/TOOLCHAIN
Game → Editor/Authoring UI
Render → ECS
```

## 8. 架构检查脚本

### 8.1 Include 扫描

CREATE `tools/architecture/check_includes.py`：

```python
FORBIDDEN = {
    "modules": [
        "lux/engine/runtime/",
        "lux/engine/editor/",
        "lux/engine/hosts/",
        "lux/ecs/",
    ],
    "ecs": [
        "lux/engine/editor/",
        "lux/game/",
    ],
}
```

扫描：

- public headers；
- source includes；
- generated headers；
- forwarding headers单独 allowlist。

### 8.2 CMake Link Closure

CREATE `tools/architecture/check_target_graph.py`，输入 CMake File API codemodel JSON：

```text
build/.cmake/api/v1/reply/target-*.json
```

检查每个 target 的递归依赖分类。

### 8.3 安装闭包

对安装前缀执行：

```text
Config package 搜索
动态库依赖 inventory
头文件 forbidden token
build path leak
```

### 8.4 用户语义扫描

对导出游戏目录拒绝：

```text
editor
toolchain
authoring
EngineRuntime
EditorRuntime
engine_pak
```

对公共 Modules SDK 拒绝：

```text
SceneRuntime
GameApplication
EngineExtensions
RuntimeContributionRegistrar
```

## 9. Build-time Tool 处理

### 9.1 Host Tools

跨编译时，Meta Generator、Shader Emitter、Asset Packer 使用 `LUX_HOST_TOOLS_PREFIX`。公共 Runtime Config 只需要生成结果，不在消费者机器重新运行生成器，除非用户主动使用 Toolchain SDK。

### 9.2 CMake Script 归属

| 当前 | 目标 |
| --- | --- |
| `engine_add_meta.cmake` | `cmake/Codegen/Reflection.cmake` |
| render comm operation scripts | `cmake/Codegen/RenderOperations.cmake` |
| shader emit scripts | `lux-render-toolchain` 或 build tool package |
| `add_asset.cmake` | `lux-resource` 的 Asset Build Helpers |
| runtime inventory | products/export tooling |

Build Helper Package 与 Runtime Library Package 分开安装。

## 10. Profile 重构

保持现有：

```text
DEVELOPER
PLAYER
EDITOR
TOOLCHAIN
```

新增：

```text
MODULES_SDK
ECS_SDK（可选）
```

Profile 必须用显式目录列表，不使用自动目录枚举。

### 10.1 Profile 矩阵

| Profile | Modules | ECS | Engine | Authoring | Toolchain | Editor | Products |
| --- | --- | --- | --- | --- | --- | --- | --- |
| MODULES_SDK | ✓ | — | — | — | — | — | samples |
| PLAYER | ✓ | ✓ | ✓ | — | — | — | player |
| EDITOR | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | editor |
| TOOLCHAIN | 最小集 | schema 需要 | format 需要 | ✓ | ✓ | — | tools |
| DEVELOPER | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | all |

## 11. 输出文件名

外部库：

```text
lux_render
lux_asset
lux_input
```

Engine 产品内部：

```text
lux_engine_scene
lux_engine_game
lux_editor
```

导出游戏可执行文件使用项目名，而不是 `lux_player`。参考 Player 可执行仍可叫 `lux_player`，Exporter 复制/重命名为游戏名。

## 12. ABI 与 Symbol Visibility

### 12.1 Visibility Header

每个公开包生成领域路径：

```text
lux/render/visibility.hpp
lux/asset/visibility.hpp
```

不再复用：

```text
lux/engine/function/visibility.h
lux/engine/resource/visibility.h
```

### 12.2 ABI 测试

Extension ABI：

```text
sizeof
alignof
standard_layout
symbol names
fingerprint
```

Render/Asset 公共 C++ ABI 若不承诺跨编译器稳定，应在文档中明确“同 toolchain version”合同，不假装稳定 C ABI。

## 13. 迁移脚本

CREATE：

```text
tools/migration/rewrite_includes.py
tools/migration/rewrite_targets.py
tools/migration/report_legacy_symbols.py
```

脚本只执行确定映射；无法确定的 include 生成报告，不自动猜测。

每次脚本运行后：

```text
clang-format
CMake configure
build
tests
legacy scan
```

## 14. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| BUILD-01 | 新分类与 File API graph checker | 旧图可检查 |
| BUILD-02 | 新领域 Alias，不改 package | 全构建 |
| BUILD-03 | 新 Include Prefix + forwarding headers | 旧消费者可编译 |
| BUILD-04 | Modules SDK packages | installed samples |
| BUILD-05 | Engine/ECS package 重组 | profiles |
| BUILD-06 | Build Tool package 分离 | cross compile |
| BUILD-07 | 迁移仓内所有 include/targets | legacy report 仅兼容 |
| BUILD-FINAL | 删除旧 Config/Alias/Forwarding | legacy report 为零 |

## 15. 验收闸门

- [ ] 外部 Render 样例只使用 `find_package(lux-render)`。
- [ ] 外部 Asset 样例只使用 `find_package(lux-resource)`。
- [ ] 公共 headers 使用 `lux/<domain>`。
- [ ] 旧 target 只是 alias，不是真实实现 target。
- [ ] Modules SDK Config 无 ECS/Engine。
- [ ] Build Tool 依赖不进入 Runtime Config。
- [ ] CMake File API 依赖图检查在持续集成运行。
- [ ] 安装前缀无 build-tree absolute path。
- [ ] Legacy include/target report 最终为零。
- [ ] 导出游戏 inventory 无 Editor/Toolchain/Authoring。


---

# 测试、持续集成、分阶段 Pull Request 与验收

> 用可自动执行的测试和依赖闸门保证大规模迁移始终可构建、可回滚且不引入新的语义下沉

**执行文档 10 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | 持续集成、测试、发布、各工作流负责人和最终验收人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 验收更新：** AssetStore/新 Asset ID 测试目标不再施工。新增 Scene Asset outer-header/legacy LXSC、Catalog magic 冲突、显式 boot Scene、ECS 领域 Payload owner 与安装反向查找契约；内部 LXSC/LXES/LXTT/LXTC/LXPC Golden 必须保持不变。详见 `ADR-20260820_SceneAsset与Resource边界.md`。

> **2026-08-21 验收更新：** 增加 manager-less 全资产 decode、同步/异步等价、shell 替换账本不变、Runtime 无同步 IO、Registry owner 与 Meta installed closure 契约。Modules 独立性不通过新增 Profile 验证，而通过现有安装包的独立 consumer 验证。

> **2026-08-21 Component Archive 验收：** Core byte archive、ECS Component Archive、Unknown Component 三路径、资产 UUID annotation 与 Function Animation 闭包已经 owner/installed consumer 验证；四 Profile 全量与第二轮 no-op 通过。当前构建树仍未注册 CTest 条目，因此不得把“0 tests”写成 CTest 覆盖。

> **2026-08-21 Extension ABI 验收：** v4 size/alignment/offset/ordinal/fingerprint/symbol、path/memory DLL loading、dependency、rollback、reflection publication、lease/unload、Editor-only registrar 与 Physics2D exports 已验证；四 Profile、installed consumer、旧 component 反向查找和三安装前缀同步通过。


## 1. 总体策略

本次重构必须以“每个阶段都可发布”为目标，不建立长期不可构建分支。测试分为五层：

```text
L0  架构静态闸门
L1  纯库单元测试
L2  领域集成测试
L3  产品行为测试
L4  安装/导出/跨平台验收
```

每个 Pull Request 至少覆盖其影响层，不允许只以“全仓能编译”作为完成标准。

## 2. 基线采集

CREATE `tools/baseline/collect_refactor_baseline.py`，记录：

```text
目标数量
安装组件数量
公共头数量
递归依赖边数
各 Profile 配置时间
全量构建时间
增量二次构建是否 no-op
主要二进制尺寸
启动/关闭时间
Scene load 时间
Asset load latency
Renderer frame CPU/GPU 时间
Editor project open 时间
```

输出：

```text
artifacts/refactor-baseline/<commit>/<profile>.json
```

重构验收比较功能与趋势，不要求所有指标单调下降，但新增概念和依赖必须有解释。

## 3. L0：架构静态闸门

### 3.1 Modules Include Gate

扫描：

```text
modules/**/include
```

禁止：

```text
lux/engine/runtime/
lux/engine/editor/
lux/engine/hosts/
lux/ecs/
SceneRuntime
EngineExtensions
GameApplication
RuntimeContributionRegistrar
```

### 3.2 Dependency Graph Gate

使用 CMake File API 递归检查：

```text
MODULE → ECS/ENGINE/EDITOR/TOOLCHAIN  禁止
ECS → ENGINE/EDITOR                  禁止
GAME → EDITOR                        禁止
RENDER → ECS                         禁止
```

### 3.3 Resource Gate

持续集成断言：

```text
modules/resource 的一级子目录集合 == {description, asset}
```

并扫描 `description` 禁止：

```text
ExtensionId
SceneFeature
GameManifest
RendererHandle
EntityRegistry
Editor
```

### 3.4 Naming Gate

新文件/类型拒绝：

```text
*Host
*Manager
*Controller
*Runtime
*Service
```

除非在 `architecture-name-exceptions.json` 中登记：

```json
{
  "ScriptRuntime": "language execution environment",
  "FileWatcher": "not matched",
  "PhysicsSystem": "ecs::ISystem"
}
```

例外必须包含理由和 owner。

### 3.5 Legacy Gate

迁移期间生成趋势报告，最终拒绝：

```text
lux::engine::function::
lux::engine::resource::
lux::engine::runtime::
asset_id_t
EAssetType（legacy codec 目录除外）
GameHost
LuxEditor::Runtime
EditorAsyncService
```

## 4. L1：公共模块单元测试

### 4.1 Core

```text
Events ordering / unsubscribe / reentrancy
Log sink thread safety
Math position/grid conversion
Serialization bounds/truncation/unknown fields
```

### 4.2 Platform

```text
DynamicLibrary path and memory load
FileWatcher create/modify/delete/coalescing
Window core state without Vulkan
GLFW backend event translation
```

### 4.3 Resource

```text
AssetId and AssetTypeId collision handling
Header probe
CodecRegistry duplicate/collision
Reader truncation
Provider mount precedence
VFS normalization
Pak index corruption
Legacy EAssetType adapter
```

### 4.4 Function

```text
Render graph topology and hazards
Render protocol bounded queues
Input mapping/context precedence
Animation sampling
Navigation codec/query
Script load/invoke/unload
UI core panel lifecycle without GLFW/Vulkan
```

## 5. L2：领域集成测试

### 5.1 ECS

```text
Schedule topology
System install/remove safe point
generation handle stale rejection
SceneServices resolve/unload
component schema draft commit
entity section decode/materialize
unknown component skip/reject policy
```

现行策略由 Component Archive ADR 固定：未知字段只在已知 Component 的 compatible
Authoring payload 中跳过；未知 Component schema 在 Authoring、Toolchain 与 Runtime
一律拒绝，Cooked LXES 使用 exact field contract。

### 5.2 AssetStore

```text
deduplicated concurrent load
failure fan-out
generation after eviction
budget eviction
close with accepted work
provider failure
codec failure
```

### 5.3 Extension

```text
ABI descriptor validation
dependency graph cycle
missing dependency
duplicate schema/feature/operation
hash collision
failure before publish
failure after operation installation rollback
unload with live lease rejection
unload after Scene close
reflection draft rollback
```

### 5.4 Scene

```text
Feature dependency closure
Feature transaction rollback
Schedule ownership
startup section load
generated section
streaming section
close while load pending
headless Scene
rendered Scene
```

### 5.5 Render

```text
Renderer open failure rollback
surface loss
swapchain resize
upload accepted before close
close with pending frame
offscreen view
multiple scene views
standard features
tooling features absent from standard package
```

## 6. L3：产品行为测试

### 6.1 Game

```text
manifest load
base/game pack mount
required extension load
boot package
frame loop
input
script start
scene close
product close
```

### 6.2 Editor

```text
start without project
open/close/switch project
Content scan
import/create/delete/move/rename
open/edit/save/close each Document
dirty confirmation
Edit → Play → Edit
Play close during loading
Preview + Play coexist
extension editor panels
```

### 6.3 Flow

```text
new graph
open legacy asset
edit/undo/redo
save
compile snapshot
latest-wins
close during compile
no NodeRegistry::global
```

### 6.4 Material

```text
graph material
material instance chain
compile failure/success
latest-wins
preview
texture binding
save in place/save as
close during compile
```

### 6.5 Preview/Thumbnail

```text
PreviewScene open/close
material preview update
thumbnail queue
cache invalidation
renderer close ordering
```

## 7. L4：安装、导出与平台

### 7.1 Installed SDK Samples

每个样例在全新构建目录使用安装前缀：

```text
modules_minimal
render_minimal
asset_minimal
input_minimal
script_minimal
ui_minimal
ecs_minimal
extension_minimal
```

不能通过源树 target 泄漏未安装 include。

### 7.2 Profile Matrix

必须覆盖：

```text
MODULES_SDK
PLAYER
EDITOR
TOOLCHAIN
DEVELOPER
```

平台：

```text
Windows x64
Linux x64
Android arm64
```

macOS 若当前无持续集成，至少保留配置级检查和发布前人工验证。

### 7.3 双构建

每次 CMake 或 Codegen 变更：

```bash
cmake --build <build-dir>
cmake --build <build-dir>
```

第二次必须无不必要工作，防止生成器 timestamp 循环。

### 7.4 Export Inventory

Exporter 输出后扫描：

```text
允许：game executable、base/game packs、runtime libs、required extensions
拒绝：editor、toolchain、authoring、generator、source asset、build paths
```

## 8. Sanitizer 与故障注入

### 8.1 Sanitizer

分别运行：

```text
AddressSanitizer（地址消毒器）
UndefinedBehaviorSanitizer（未定义行为消毒器）
ThreadSanitizer（线程消毒器，单独配置）
```

重点路径：

```text
Extension load/unload
Scene close
Asset eviction
Renderer close
Editor Play exit
Preview destruction
Panel close during async completion
```

### 8.2 故障注入点

建立统一 test-only injector：

```cpp
enum class FailurePoint
{
    afterExecutorOpen,
    afterRendererThreadStart,
    afterAssetStoreOpen,
    afterExtensionLibraryLoad,
    afterOperationInstall,
    afterSceneServiceCreate,
    afterSystemCreate,
    afterFeatureValidate,
    afterFrameAccept,
};
```

每个构建事务必须测试所有中间失败点的回滚。

### 8.3 超时

关闭测试使用确定性 fake clock 或 bounded poll；不以随机 sleep 判断完成。

## 9. 文件格式与 ABI 测试

### 9.1 Golden Files

版本控制保存：

```text
assets/v1-vN
pak/v1-vN
ecs_section/v1-vN
scene_package/v1-vN
game_manifest/v1-vN
```

每次格式修改必须：

```text
old reader test
new reader test
conversion test
corruption rejection
canonical encoding
```

### 9.2 Extension ABI

编译一个最小 C Extension fixture 和一个 C++ Extension fixture，验证：

`ADR-20260821_ExtensionAbiV4Owner与Core清零.md` 进一步要求 owner 搬迁前后逐项固定
v4 descriptor size/alignment/offset、枚举 ordinal、ABI fingerprint、registrar ABI-facing
类型与三个导出 symbol string。installed consumer 只查找 Engine Extension SDK；旧 Core
`extension_abi` component 与 include 必须失败/不存在。

```text
symbol lookup
descriptor size/alignment
fingerprint mismatch
version range
runtime/editor target
register failure
unload
```

### 9.3 Script ABI

单独 fixture；不得复用 Extension ABI fixture。

## 10. 性能与容量

### 10.1 不变量

重构不得：

```text
把每帧查询从 O(1) 变成 service map lookup
在热路径引入 shared_ptr 原子增减
把 bounded queue 改为 unbounded
在 Render submit 重新引入全局 mutex
让 Scene Feature 每帧解析依赖
```

### 10.2 基准

```text
Schedule update
SceneServices resolved access
AssetHandle lookup
Render packet submit
Main-thread completion drain
ContentIndex scan
Document compile snapshot
```

保留阈值和趋势，不以单次波动失败；连续回归超过阈值需要解释。

## 11. Pull Request 路线

### M0：架构闸门

```text
SDK-01
BUILD-01
baseline collection
```

退出：能自动发现新的语义下沉。

### M1：Core/Platform

```text
CORE-02..05
PLATFORM-01..03
```

退出：Extension ABI、common、spatial 归位；GAPI 按保留 ADR 继续存在。

### M2：Resource/Function

```text
RES-01..08
RENDER-01..05
INPUT/ANIM/NAV/SCRIPT/UI
```

退出：Modules SDK 独立安装样例通过。

### M3：ECS

```text
ECS-01..07
```

退出：ECS Core 依赖闭包纯净，Scene Format/Package 分开。

### M4：Engine

```text
EXEC/ASSET/EXT/SCENE/RENDER-ENG/FRAME
```

退出：engine/runtime 聚合层可删除。

### M5：Products

```text
SESSION/PRODUCT/MANIFEST/EXPORT
```

退出：GameHost 删除，Editor Play 复用 Session。

### M6：Editor

```text
EDIT/FLOW/MAT/SCRIPT/PREVIEW
```

退出：Runtime/Hook/Controller 网络删除。

### M7：SDK 与兼容归零

```text
BUILD-03..FINAL
```

退出：旧 target/include/package 为零。

### M8：发布验收

所有 Profile、平台、Sanitizer、Exporter 与安装样例通过。

## 12. 每个 Pull Request 的提交前清单

- [ ] 变更只处理一个明确边界。
- [ ] 当前与目标依赖闭包已附图。
- [ ] MOVE/SPLIT/DELETE 文件清单完整。
- [ ] 新 target 分类正确。
- [ ] 新 public header 独立自包含。
- [ ] 无新的宽泛 Context/Host/Manager/Service。
- [ ] 无新的 service locator。
- [ ] 无不必要 shared_ptr。
- [ ] 失败路径使用 expected/结构化 error。
- [ ] 构造失败能回滚。
- [ ] close 路径有测试。
- [ ] 文件格式/ABI 变化已单独说明。
- [ ] 兼容层有删除里程碑。
- [ ] 受影响 Profile 已配置、构建、测试。
- [ ] 二次构建 no-op。
- [ ] 架构扫描通过。

## 13. 最终完成定义

- [ ] 00–09 文档中的目标目录和 target 已实现。
- [ ] 11 Checklist 全部勾选或有批准的永久例外。
- [ ] 12 映射表中无 `PENDING`。
- [ ] Modules SDK 可完全独立构建和安装。
- [ ] Resource 只有 Description/Asset。
- [ ] ECS Core 无 Engine 依赖。
- [ ] Extension API 不在 Modules。
- [ ] Game/Editor 无公开 Runtime 容器。
- [ ] Editor 无必需依赖 hook。
- [ ] 旧包名、target、include、namespace 已删除。
- [ ] 导出游戏无 Editor/Toolchain/Authoring 与用户可见 EngineRuntime 语义。


---

# LUX Engine 重构详细施工 Checklist

> 按里程碑、目录、类型、CMake、测试与删除闸门列出的可勾选实施清单

**执行文档 11 · 重构实施版 v2.13（Asset Pipeline / Core Meta 施工裁决）**

| 项目 | 内容 |
| --- | --- |
| 原始代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 当前实施分支 | `codex/asset-pipeline-core-meta-boundary` |
| 当前实施 Head | 施工基线 `fe4422ba`；代码尚未开始 |
| 上游配套提交 | lux-cxx `91b9233713bb713adeb16acaf681a84dd36e4546`；lux-cmake-toolset `961c63eda82448b8108219461ba624ff016b2297` |
| 最近维护者全量构建通过 | Windows x64 / MSVC / RelWithDebInfo，Asset 领域内聚至 `e7348155` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-21 |
| 适用对象 | 项目经理、技术负责人、各领域实施者、Pull Request 评审者、持续集成维护者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> `ADR-20260820_SceneAsset与Resource边界.md` 已取代新建 `AssetId`、`AssetTypeId`、Engine `AssetStore` 和把场景 Payload 永久留在 Resource 的旧目标。被取代项保持未勾选并标记 `SUPERSEDED`，不伪装为已完成，也不再进入待施工统计。

> `ADR-20260821_Asset运行期需求与SerDeser边界.md` 与 `ADR-20260821_CoreMeta纯化与ECSRegistry归位.md` 已裁决并完成本阶段目标。`ASSETPIPE-*` 的完成证据记录在本清单末尾、迁移映射与事实索引中。

## 2026-08-20 施工状态更新

| 项目 | 当前状态 |
| --- | --- |
| 当前施工结果 | Resource 一级内容与 Spatial component 已全部退役；`modules/resource` 一级目录和注册只剩 `description` 与 `asset` |
| Platform Common | `modules/platform/common` 已退役；AtomicWait/Format 归入 lux-cxx，Extent 归 Core Math，Image 枚举归 Description，Texture Role 归 Render Graph；无 alias、shim 或 forwarding header |
| 格式兼容 | LXSC v1、LXES v1、Persistence Journal v1、L3SC v1 使用 canonical owner 的版本化 Golden fixture；LXWA v4 及 LXAI/LXAD/LXIP/LXTP/LXTL/LXPP 子文档使用固定字节长度与 SHA-256 指纹，均验证 decode/re-encode 字节不变 |
| 身份与 DTO | Authoring 已拥有 `WorldId`、`WorldActorId`、`WorldSceneFeatureRequest` 与 `WorldRequiredExtension`；Authoring→cooked 只在 Toolchain leaf 执行显式值转换 |
| 当前实施分支 | `codex/asset-domain-cohesion`，代码基线 `f35e245a`，owner 迁移 `1364810c`，契约 `e7348155` |
| 已验证 | Windows x64 / MSVC / RelWithDebInfo 的 DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 全量构建通过，各 CMake 变更构建树第二轮均为 `ninja: no work to do`；owner contract/public-link/roundtrip/export smoke 全部通过 |
| 边界门禁决策 | 项目决定删除四项 `check_*boundary.py` 及对应 GitHub Actions Job，边界改由 owner 编译/链接/Golden/Wire 契约测试负责；现有 CMake target DAG 门禁保留 |
| 当前施工范围 | Asset 领域族、Storage/Pak 公共边界、Engine Content owner 与 ECS fallback 注入已完成；Registry、Editor 架构重构、M0 和全局 M7 不推进 |


> 勾选规则：`[x]` 只表示该条目的目标所有者、代码、CMake 与对应验证已经建立；带“兼容层仍保留”的项目，旧入口的最终删除由 `FINAL-*` 条目单独追踪。


## 使用方法

- 每个条目都有稳定 ID；Issue、Pull Request 与提交信息应引用这些 ID。
- 条目只有在代码、测试、CMake、安装导出与文档同时完成后才能勾选。
- 暂缓项使用 `BLOCKED: <原因>/<owner>/<复审日期>`，不能直接删除。
- 永久不实施的项必须通过架构评审，并移入例外清单。
- 里程碑按 00 文档规定顺序推进；上层工作可做原型，但不能在下层边界未稳定前删除旧路径。

## 全局施工纪律

- [x] `GLOBAL-001` 确认实施分支基于或包含 `09b2a82582550bcbe03afeef77d2591e1656a656`；若前移，记录新基线提交。 **完成：实施分支包含原始基线；当前 Head 为 `b1a25d3bb23f33f092964465c7d27d819beaf7db`。**
- [ ] `GLOBAL-002` 冻结新增 `Host`、`Manager`、`Controller`、`Runtime`、`Service` 类型，除非架构评审登记例外。
- [ ] `GLOBAL-003` 禁止新建 `EngineRuntime`、`EditorRuntime`、`GameRuntime` 或任意 `get<T>()` 服务容器。
- [ ] `GLOBAL-004` 禁止把共享上层格式下沉到 `modules/resource` 以规避依赖环。
- [ ] `GLOBAL-005` 禁止让 `modules` PUBLIC 链接 ECS、Engine、Editor、Toolchain。
- [ ] `GLOBAL-006` 禁止新代码 include 旧 `lux/engine/function`、`lux/engine/resource` 公共前缀。
- [ ] `GLOBAL-007` 禁止新业务代码使用 `asset_id_t`、`EAssetType`。 **SUPERSEDED：ADR-20260820 保留这两个既有 wire/API 类型；后续只禁止在 Resource 中增加 Engine-owned 枚举名称。**
- [ ] `GLOBAL-008` 禁止新增必需依赖 setter 或 `set*Hook()`。
- [ ] `GLOBAL-009` 禁止以 `shared_ptr` 代替明确父子生命周期引用。
- [ ] `GLOBAL-010` 每个移动 PR 附当前与目标依赖闭包。
- [ ] `GLOBAL-011` 每个 compatibility alias 在创建时登记删除里程碑。
- [ ] `GLOBAL-012` 每个格式变化与目录/命名迁移分开提交。
- [ ] `GLOBAL-013` 每个异步 owner 明确 TaskGroup/close 顺序。
- [ ] `GLOBAL-014` 每个动态 Extension 对象明确 ExtensionLease。
- [ ] `GLOBAL-015` 每个 Panel 构造后立即合法可用。
- [ ] `GLOBAL-016` 每个 public header 单独编译自包含测试。
- [x] `GLOBAL-017` 每个 CMake 变更执行两次构建并确认第二次 no-op。 **完成（本波次）：DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的最终第二轮构建均输出 `ninja: no work to do`。**
- [x] `GLOBAL-018` 每个 PR 执行受影响 Profile 配置、构建和测试。 **完成（本波次）：四个受影响 Profile 均完成 Windows x64 / MSVC / RelWithDebInfo 配置、全量 `target all` 构建与相应 CTest/owner contract tests。**
- [x] `GLOBAL-019` 每个删除动作先通过全仓符号搜索与 target graph 检查。 **完成（本波次）：两个旧 Resource include、namespace、target/component 的 production/test/CMake 全仓搜索归零，现有 CMake DAG 门禁通过。**
- [ ] `GLOBAL-020` 每个永久例外写入 `architecture-exceptions.json` 并包含 owner、原因和复审日期。
## M0：公共 SDK 架构闸门

- [ ] `SDK-001` 创建 `MODULES_SDK` Profile，完全跳过 `ecs/`、`engine/` 和 products。 **SUPERSEDED：合法 Profile 仍只有 DEVELOPER/PLAYER/EDITOR/TOOLCHAIN；用 installed consumers 验证 Modules SDK。**
- [x] `SDK-002` 根 CMake 为 modules、ecs、engine、products 建立显式开关或 Profile 门控。 **等价完成：合法 Profile 仍为 DEVELOPER/PLAYER/EDITOR/TOOLCHAIN，聚合层按 Profile 显式选择产品闭包。**
- [x] `SDK-003` 删除 modules 四个层级中的自动目录枚举。 **完成：Core、Platform、Resource、Function 均使用显式子目录清单。**
- [x] `SDK-004` 显式列出 `modules/core` 的保留子目录。 **完成：math、meta、serialization、log、events 显式配置；已退役 extension_abi 不再列入。**
- [x] `SDK-005` 显式列出 `modules/platform` 的保留子目录。 **完成：按现有 Profile/Android 条件显式配置平台组件。**
- [x] `SDK-006` 显式列出 `modules/resource` 的 `description` 与 `asset`。 **完成：Resource 根 CMake 已删除自动子目录枚举，只显式添加并安装 `description` 与 `asset` 家族。**
- [x] `SDK-007` 显式列出 `modules/function` 的领域模块。 **完成：普通 Profile 与 TOOLCHAIN 使用各自显式 Function 闭包。**
- [ ] `SDK-008` 创建 `tools/architecture/check_includes.py`。
- [ ] `SDK-009` 扫描 modules public headers 的 forbidden include。
- [ ] `SDK-010` 扫描 modules public headers 的 forbidden symbol。
- [ ] `SDK-011` 创建 CMake File API target graph checker。
- [ ] `SDK-012` 递归检查 modules target 不依赖 ECS/Engine/Editor/Toolchain。
- [ ] `SDK-013` 创建 installed modules SDK smoke project。
- [ ] `SDK-014` 创建 render-only installed sample。
- [ ] `SDK-015` 创建 asset-only installed sample。
- [ ] `SDK-016` 创建 input-only installed sample。
- [ ] `SDK-017` 创建 script-only installed sample。
- [ ] `SDK-018` 创建 UI-core-only installed sample。
- [ ] `SDK-019` 检查安装 Config 无 build-tree absolute path。
- [ ] `SDK-020` 检查安装 Config 无 generator/toolchain 依赖泄漏。
- [ ] `SDK-021` 记录当前公共 target、header、component 数量基线。
- [ ] `SDK-022` 记录当前 modules 递归依赖边基线。
- [ ] `SDK-023` 把 A1–A8 决议写入仓库 ADR。
- [ ] `SDK-024` 更新 README 的层级图与 modules 定义。
- [ ] `SDK-025` 持续集成加入 `MODULES_SDK` configure/build/test job。 **SUPERSEDED：不增加第五 Profile；在现有 Profile 安装结果上运行独立 consumer。**
## M1：Core 清理

- [x] `CORE-001` 在 `engine/extensions/api` 创建 Extension API target。 **完成：该 target 现为 ABI v4 实体 owner，installed closure 仅导出 lux-cxx core/abi。**
- [x] `CORE-002` 移动 `ModuleAbi.hpp` 到 `ExtensionAbi.hpp`。 **完成：聚合入口与函数类型/symbol constants 由 Engine API 直接定义，旧头删除。**
- [x] `CORE-003` 移动 Extension Descriptor 与 Version 类型。 **完成：descriptor/id/version/result 拆分头为唯一 owner，旧 Core 定义删除。**
- [x] `CORE-004` 保留旧 ABI symbol string 的兼容测试。 **完成：测试固定 v4 size/alignment/offset/ordinal/fingerprint 与三个 symbol string，动态 fixture 全部通过。**
- [x] `CORE-005` 从 Core 安装组件移除 `extension_abi`。 **完成：Core available components 不再包含该项，旧 component 反向查找失败。**
- [x] `CORE-006` 删除通用 `ContributionId`。 **完成：类型/helper 及 production/test/CMake 精确词引用归零，不建立替代通用 ID。**
- [x] `CORE-007` 在 ECS 定义 `ComponentSchemaId`。 **完成：新增 `ecs/identity/include/lux/engine/ecs/ComponentSchemaId.hpp`。**
- [ ] `CORE-008` 在 Render 定义 `FeatureId/RepresentationId`。 **进行中：`RenderEffectId` 已完成，且 Render Effect 对 Scene Feature 的依赖已强类型化；`RepresentationId` 仍待迁移。**
- [x] `CORE-009` 在 Engine Scene 定义 `FeatureId`。 **完成：新增 Engine-owned `lux::scene::SceneFeatureId`，Runtime Catalog/Ticket/Snapshot 已迁移。**
- [x] `CORE-010` 在 Engine Extensions 定义 `ExtensionId`。 **完成：`engine/extensions/api/ExtensionId.hpp` 为唯一实体 owner。**
- [x] `CORE-011` 拆分 `LuxObject.hpp` 中 Entity Registry。 **完成：旧聚合头删除，Entity、Registry 与 allocator 成为 ECS Core 独立公共头。**
- [x] `CORE-012` 把 `EntityRegistry` 移入 `ecs/core`。 **完成：最终 API 为 `lux::ecs::RegistryBase/Registry/EntityHandle/ConstEntityHandle`，无旧 alias。**
- [x] `CORE-013` 评估并删除 `LuxObject` 基类。 **完成：Asset 与反射 record 均不再依赖 OO 根类。**
- [x] `CORE-014` 评估并删除 `EntityObject`；若保留，改名并限制作用域。 **完成：删除，不建立替代 wrapper。**
- [x] `CORE-015` 把 Runtime Reflection 纯类型与 ECS Entity 解耦。 **完成：Meta 只处理标注 record/external reflected value；EnTT component 操作归 ECS adapter。**
- [ ] `CORE-016` 把通用 Reflection 优先合入 `lux-cxx::reflection_runtime`。
- [ ] `CORE-017` 把 Extension reflection publication 移入 Engine。
- [ ] `CORE-018` 移动 `engine_add_meta.cmake` 到 Build Tool 目录。
- [x] `CORE-019` 确认 Runtime targets 不 PUBLIC 链接 meta generator。 **完成：安装与 Profile 闭包验证未通过 Runtime PUBLIC 边传播生成器。**
- [x] `CORE-020` 拆分 Serialization 基础 archive 与 reflected adapter。
- [x] `CORE-021` 从 `lux::serialization` PUBLIC 依赖移除 EnTT。 **完成：Core Meta/Serialization 源码、链接与 installed consumer 闭包均无 EnTT。**
- [x] `CORE-022` 从 `lux::serialization` PUBLIC 依赖移除 Engine Reflection。
- [x] `CORE-023` 建立 ECS Component Archive adapter。
- [ ] `CORE-024` 建立 Registry Archive adapter。 **SUPERSEDED：不序列化 `entt::registry`；使用 `EntitySectionImage -> EntityBatchStager -> Registry` staged materialization。**
- [x] `CORE-025` 补充 Serialization truncation/unknown-field tests。
- [ ] `CORE-026` 补充 Reflection draft commit/rollback tests。
- [ ] `CORE-027` 补充 Entity Registry memory tests。
## M1：Platform 清理

- [x] `PLATFORM-001` 列出 `platform/common` 所有文件和消费者。 **完成：AtomicWait、Format、Size2D、ImageEnums 及全仓 include/link/install 消费者已盘点并迁移。**
- [x] `PLATFORM-002` 把 `AtomicWait` 迁入 lux-cxx concurrent 或精确 Core 模块。 **完成：唯一 owner 为 `lux::cxx::concurrent::waitAtomicU64Until`，Windows/Android 编译与行为契约通过。**
- [x] `PLATFORM-003` 把 Format compatibility 迁入 lux-cxx format 或精确 Core 模块。 **完成：`lux/cxx/core/Format.hpp` 保持 std::format/libfmt/fallback 选择语义，旧生成头已删除。**
- [x] `PLATFORM-004` 把 `Size2D` 迁入 Math Extent。 **完成：唯一 owner 为 `lux::math::Extent2u`；布局固定为两个 `uint32_t`、8 字节，旧类型和旧头归零。**
- [x] `PLATFORM-005` 把 `ImageEnums` 迁入 Description Image。 **完成：Texture Dimension/Format 归 `lux::rdesc`，Render-only Texture Role 归 `lux::render`；枚举底层宽度和 ordinal 不变，旧聚合头归零。**
- [x] `PLATFORM-006` 迁移所有 `platform::common` PUBLIC links。 **完成：消费者改为按用途直接依赖 lux-cxx Core/Concurrent、Core Math、Description 或 Render Graph。**
- [x] `PLATFORM-007` 删除 `platform/common` target。 **完成：源码目录、CMake target 与全部旧 namespace/include 引用已删除，无兼容 alias。**
- [x] `PLATFORM-008` 删除 `platform/common` install component。 **完成：三个安装前缀旧头/导出产物归零，旧 component 的 `find_package` 明确失败。**
- [ ] `PLATFORM-009` 移动 `platform/gapi` Vulkan wrappers 到 render/vulkan。 **SUPERSEDED：ADR-20260821 保留 GAPI 公共 SDK，不迁移 owner。**
- [ ] `PLATFORM-010` 消除 gapi 与 render/vulkan 重复 Buffer/Image/Device 抽象。 **SUPERSEDED：GAPI wrapper 与 Render 内部 handle 服务不同公共边界，本重构不强制合并。**
- [ ] `PLATFORM-011` 删除 `platform::gapi` target。 **SUPERSEDED：target/component/include/namespace 永久保留。**
- [ ] `PLATFORM-012` 拆 `window` 为 core 与 backend。
- [ ] `PLATFORM-013` 创建 `window_glfw` target。
- [ ] `PLATFORM-014` 创建 Android window backend target 或保持条件源但不污染 core。
- [ ] `PLATFORM-015` 将 GLFW 依赖改为 backend PRIVATE。
- [ ] `PLATFORM-016` 公共 Window 头移除 Vulkan include。
- [ ] `PLATFORM-017` 创建 NativeWindowHandle 抽象。
- [ ] `PLATFORM-018` 创建 Vulkan Window Surface integration target。
- [ ] `PLATFORM-019` 迁移 `GlfwRuntime` 为 `GlfwLibrary` 或 backend internal state。
- [ ] `PLATFORM-020` 移动 `TrayIconWin32` 到 Launcher 或独立 tray 模块。
- [ ] `PLATFORM-021` 补充 Window core 无 Vulkan 构建测试。
- [ ] `PLATFORM-022` 补充 Window raw event → 单体 Input platform implementation 的翻译测试。
- [ ] `PLATFORM-023` 补充 DynamicLibrary path/memory load 测试。
- [ ] `PLATFORM-024` 补充 FileWatcher coalescing/close 测试。
## M2：Resource Description

- [ ] `DESC-001` 创建新的 `lux/description` include prefix。
- [ ] `DESC-002` 创建单一 `lux::description` 真实 target。
- [ ] `DESC-003` 把 Mesh/Vertex/Model 文件整理到 description/mesh。
- [ ] `DESC-004` 把 Texture/Image/Atlas 文件整理到 description/image。
- [ ] `DESC-005` 把 Skeleton/AnimationClip 文件整理到 description/animation。
- [ ] `DESC-006` 把 Material pure values 整理到 description/material。
- [x] `DESC-007` 把 Terrain pure values 合入 description/terrain。 **完成：`TerrainTileBlobV1` 与布局常量归入 `lux/engine/description/TerrainTile.hpp`；LXTT v1 Codec 归入 `asset`。**
- [x] `DESC-008` 把 Tilemap pure values 合入 description/tilemap。 **完成：`TilemapChunkBlobV1` 与布局常量归入 `lux/engine/description/TilemapChunk.hpp`；LXTC v1 Codec 归入 `asset`。**
- [x] `DESC-009` 把 StaticCollider pure values 合入 description/physics。 **完成：`StaticColliderBatch3DBlobV1` 及嵌套值类型归入 `lux/engine/description/StaticColliderBatch3D.hpp`；LXPC v1 Codec 归入 `asset`。**
- [x] `DESC-010` 移动 `LayoutContract` 到 render/shader。 **完成：`LayoutContract` 迁入 Render Graph/Shader 所有者。**
- [ ] `DESC-011` 移动 `RenderRepresentation` 到 render。
- [ ] `DESC-012` 移动 `ImportedMaterialDesc` 到 toolchain/model_import。
- [ ] `DESC-013` 拆分 `MaterialGraphContract` 的 authoring/runtime/toolchain 部分。
- [ ] `DESC-014` 移动 Script pure values 到 script module。
- [x] `DESC-015` 移除 Description 对 Extension ABI 的依赖。 **完成：Description CMake 删除未使用的 Extension ABI link/export 闭包。**
- [ ] `DESC-016` 移除 Description 对 Runtime Reflection 注解的硬依赖。
- [ ] `DESC-017` 把 reflection metadata 放到 owner sidecar。
- [ ] `DESC-018` 删除 description 中 `org.lux.builtin.*` 常量。
- [ ] `DESC-019` 为每个 pure value header 增加 standalone compile test。
- [ ] `DESC-020` 使用私有 Object Library 而非增加公开 component。
- [x] `DESC-021` 删除 classic_mesh/terrain/tilemap/physics3d 一级 description components。 **完成：四个 Resource 一级 component、target、旧 include prefix 与安装产物均已删除；Classic Mesh 由 `render_standard_content` 所有，其余值/Codec 分别由 `description`/`asset` 所有。**
- [x] `DESC-022` 验证 `modules/resource` 一级目录只有 description/asset。 **完成：源码目录、CMake 注册与安装 component 清单均只保留 `description`/`asset`。**
## M2：Resource Asset

- [ ] `ASSETSDK-001` 创建 `AssetId` 强类型并保留 legacy alias。 **SUPERSEDED：保留 `asset_id_t = uuids::uuid`，不增加同义类型。**
- [ ] `ASSETSDK-002` 创建开放 `AssetTypeId`。 **SUPERSEDED：保留具有稳定显式数值的 `EAssetType` 与 Engine-owned 兼容数值。**
- [ ] `ASSETSDK-003` 为标准 Texture/Mesh/Material 等定义领域 AssetTypeId。
- [ ] `ASSETSDK-004` 为 Authoring Graph 类型在 Authoring 层定义 ID。
- [ ] `ASSETSDK-005` 建立 `Legacy EAssetType ↔ AssetTypeId` adapter。 **SUPERSEDED：不创建 `AssetTypeId`，因此不创建 adapter。**
- [ ] `ASSETSDK-006` 禁止新业务 switch `EAssetType`。
- [ ] `ASSETSDK-007` 定义新的 `AssetHeader` 外观。
- [ ] `ASSETSDK-008` 保持旧 Asset Header wire reader。
- [x] `ASSETSDK-009` 移除 `LuxAsset : LuxObject`。 **完成：`LuxAsset` 为独立多态资产基类，Asset 安装闭包不含 Core Meta。**
- [x] `ASSETSDK-010` 移除公共 `void* rawData()`。 **完成：`TAsset<T>::data()` 只返回领域 typed pointer。**
- [x] `ASSETSDK-011` 建立 `DecodedAsset` 或等价 type-erased owning result。 **等价完成：Catalog 返回 owning `std::unique_ptr<LuxAsset>`，Manager 在主线程安全点安装。**
- [x] `ASSETSDK-012` 建立 `Codec` 接口。 **等价完成：既有 `AssetSerDeser/TAssetSerDeser` 是唯一具体 Codec 多态接口。**
- [x] `ASSETSDK-013` 建立开放 `CodecRegistry`。 **等价完成：`AssetCodecCatalog` 提供 descriptor build、type/magic/C++ identity 冲突校验与查找。**
- [x] `ASSETSDK-014` 建立 `Provider` 接口。 **完成：`storage/AssetProvider.hpp` 公开 `IAssetProvider` 与 opaque `AssetBlob/ProviderEntry`。**
- [x] `ASSETSDK-015` 建立 `Vfs` mount/read API。 **完成：`AssetVfs` 覆盖 mount priority、override、tombstone、resolve/open/enumerate。**
- [ ] `ASSETSDK-016` 把 Asset Cache/Residency 从公共 Asset 移出。 **SUPERSEDED：公共 AssetManager 的通用驻留、票据、revision、驱逐与广播保持不变。**
- [ ] `ASSETSDK-017` 把 `AssetManager` 源移动到 Engine AssetStore。 **SUPERSEDED：不建立第二套 Engine AssetStore。**
- [ ] `ASSETSDK-018` 把 `AssetEvents` 移入 Engine assets 或 Content。 **SUPERSEDED：事件继续属于现有通用 AssetManager 契约。**
- [x] `ASSETSDK-019` 将 Codec 按领域分目录。 **完成：Asset、SerDeser、私有 Description Codec、源码与测试按 texture/material/mesh/model/animation/shader/script/storage 镜像组织。**
- [x] `ASSETSDK-020` 提供 `registerStandardCodecs` convenience API。 **等价完成：`runtimeAssetCodecCatalog()` 返回标准资产 Catalog。**
- [x] `ASSETSDK-021` 拆除中央 Codec switch/factory。 **完成：magic 分派统一经 `AssetCodecCatalog::findByMagic()`，中央 switch 已删除。**
- [x] `ASSETSDK-022` 收窄 `asset` 对 asset core 的依赖。 **完成：旧 asset_pak/asset_core target 已合并删除，Pak 与 Asset core 共享 canonical `lux::engine::resource::asset`，不再存在独立 Pak→Core 组件边界。**
- [x] `ASSETSDK-023` Pak Provider 不依赖 Engine AssetStore。 **完成：Pak provider/writer/inspector 全部属于公共 Asset SDK，只提供 opaque bytes，不管理 AssetRef。**
- [x] `ASSETSDK-024` 建立旧资产 Golden Files。 **完成：11 类标准资产与 LUXPAK v2 固定 length/SHA，并验证 decode/re-encode 与 AssetFileHeader v1 读取。**
- [x] `ASSETSDK-025` 建立 corrupt/truncated asset tests。 **完成：覆盖 bad magic/version、truncation、offset/tail/limit，以及 Pak header/page/payload digest 损坏。**
- [ ] `ASSETSDK-026` 建立 AssetTypeId collision tests。 **SUPERSEDED：ADR 禁止新增 `AssetTypeId`；以 Catalog 的 EAssetType、主/legacy magic 与 C++ type hash/name 冲突测试替代。**
- [x] `ASSETSDK-027` 建立 installed asset-only sample。 **完成：installed consumer 可独立 `find_package`、写入、检查并通过 Pak provider 读取。**
## M2：Render

- [ ] `RENDER-001` 建立 `lux::render` 真实 target 与 include prefix。
- [ ] `RENDER-002` 将 render_client 对外表面收敛为 Renderer/Frame/View/Upload。
- [ ] `RENDER-003` 把 RenderControl/Frame/Upload Session 内收实现。
- [ ] `RENDER-004` 删除 render_client 对 core meta 的 PUBLIC link。
- [x] `RENDER-005` 删除 render_client 对 resource deployment 的 PUBLIC link。 **完成：Render Client 已解除对 Resource Deployment 的 PUBLIC 依赖，旧 Deployment component 已删除。**
- [ ] `RENDER-006` 把 GPU capacity 迁入 render/config。
- [ ] `RENDER-007` 把 Game capacity request 留在 Game Manifest。
- [x] `RENDER-008` 让 render_graph 不依赖 platform common。 **完成：Render Graph 自有 `TextureAccess.hpp` 与 `ETextureRole`，安装 target 无 Platform Common 或其他串漏依赖。**
- [ ] `RENDER-009` 把 gapi wrappers 并入 render_vulkan low_level。 **SUPERSEDED：Render 消费保留的 GAPI，不取得 owner。**
- [ ] `RENDER-010` 统一 Vulkan Buffer/Image/Device ownership model。 **SUPERSEDED：不把公共 GAPI wrapper 与 Render 私有 handle 强制合并。**
- [ ] `RENDER-011` 拆 Window Surface integration。
- [ ] `RENDER-012` 让 render_vulkan core 不依赖 GLFW。
- [ ] `RENDER-013` 将 `RenderBackendHost` 移入 Render 并重构为 `Renderer`。
- [ ] `RENDER-014` 公共 Renderer 头不暴露 Vulkan。
- [ ] `RENDER-015` 建立 `render_standard` target。
- [ ] `RENDER-016` 把 standard feature 源拆成 private object libs。
- [ ] `RENDER-017` 把 Grid/Gizmo/Highlight 移入 render_tooling 或 Editor。
- [ ] `RENDER-018` 评估 PointCloud/Terrain/Water 是否公开可选。
- [x] `RENDER-019` 移动 LayoutContract 到 render/shader。 **完成：Render Layout Contract 已迁入 Render。**
- [ ] `RENDER-020` 移动 RenderRepresentation IDs 到 render。
- [ ] `RENDER-021` 把 codegen CMake 从 meta component 解耦。
- [ ] `RENDER-022` 保证 generated operation headers 安装完整。
- [ ] `RENDER-023` 补充 render-only installed sample。
- [x] `RENDER-024` 补充 headless render_graph build。 **完成：TOOLCHAIN/owner contract/standalone installed consumer 均可在无 Vulkan/Window 闭包下编译链接 Render Graph。**
- [ ] `RENDER-025` 补充 Renderer open failure rollback。
- [ ] `RENDER-026` 补充 surface loss/resize tests。
- [ ] `RENDER-027` 补充 close with accepted frame/upload tests。
## M2：Input/Animation/Navigation/Script/UI

- [ ] `FUNC-001` 把 PhysicalInput 与 InputSnapshot 放入 input 模块。
- [ ] `FUNC-002` 让 input target 不 PUBLIC 链接 Window/GLFW。
- [ ] `FUNC-003` 在单一 input target 内以配置期源选择实现 GLFW/Android 平台采集；禁止 Adapter target/interface。
- [ ] `FUNC-004` 保留 ActionMapper/Context 的领域逻辑。
- [ ] `FUNC-005` 建立完整 `Input` 领域对象，统一拥有 Snapshot、Mapper、唯一 ActionRegistry 与 ContextStack。
- [x] `FUNC-006` 让 animation 直接依赖 Description/Math。
- [x] `FUNC-007` 移除 animation 对 Asset Core 的依赖。
- [x] `FUNC-008` 让 navigation 依赖 Math 而非 resource/spatial。 **完成：Navigation Core 与 Detour3D 直接 include/link Core Math，安装传递依赖不再查找 Resource spatial。**
- [ ] `FUNC-009` 保留 navigation_detour3d 独立 backend。
- [ ] `FUNC-010` 将 `ScriptHost` 重命名为 `ScriptRuntime`。
- [ ] `FUNC-011` 把 invalid handle + lastError 改为 expected。
- [ ] `FUNC-012` 保持 ScriptModule 术语仅用于脚本。
- [ ] `FUNC-013` 确认 script_native 不 include Extension ABI。
- [ ] `FUNC-014` 拆 UI core 与 ImGui integration。
- [ ] `FUNC-015` 让 `lux::ui` 不依赖 GLFW/Vulkan。
- [ ] `FUNC-016` 创建 `lux::ui_imgui`。
- [ ] `FUNC-017` 创建 `lux::ui_imgui_glfw`。
- [ ] `FUNC-018` 创建 `lux::ui_render_vulkan`。
- [ ] `FUNC-019` 将 `UISystem` 重命名为 `UI`。
- [ ] `FUNC-020` 移动 SceneViewportPanel 到 Editor。
- [ ] `FUNC-021` 补充 UI core 无图形 backend 测试。
- [ ] `FUNC-022` 补充 Script Lua/Native load/invoke/unload tests。
## M3：ECS Kernel 与 Scene Format

- [x] `ECS-001` 将 Entity Registry 真实 owner 改为 ecs/core。 **完成：实现、allocator、handles、snapshot/publication 契约均归 ECS Core。**
- [x] `ECS-002` 创建 `ComponentSchemaId.hpp`。 **完成：独立 `ComponentSchemaId.hpp` 已建立。**
- [x] `ECS-003` 迁移 ComponentTypeCatalog 使用新 ID。 **完成：`ComponentTypeCatalog` 改为包含并使用 ECS-owned `ComponentSchemaId`。**
- [x] `ECS-004` 从 ecs/core 删除 Extension ABI dependency。 **完成：`ecs/core` 的直接 Extension ABI target 依赖已移除。**
- [x] `ECS-005` 从 ecs/core 删除 Asset Core dependency。 **完成：ECS Core installed consumer 闭包不导入 Resource Asset。**
- [x] `ECS-006` 从 ecs/core 删除 resource/entity_scene dependency。 **完成：`PersistentEntityIdComponent` 与 `PersistentEntityIndex` 使用 ECS-owned identity；`ecs/core` 的源码与安装期传递依赖均不再引用旧 Resource component，回归由 owner 编译契约与 CMake DAG 门禁防止。**
- [x] `ECS-007` 从 ecs/core 删除 resource/spatial dependency。 **完成：ECS 使用 `lux::math` 值类型；Runtime Reflection 由 ECS-owned external reflection adapter/traits sidecar 提供，Core Math 保持纯值。**
- [x] `ECS-008` 把 AssetLoadFn 移入 ecs/assets integration。 **按 ADR 等价完成：删除 AssetLoadFn；异步资产请求归 Engine Runtime packs/scene integration，不创建空泛 ECS integration target。**
- [x] `ECS-009` 确认 Schedule 仍唯一拥有 `unique_ptr<ISystem>`。 **完成（审计保持）：`Schedule` 仍唯一拥有 `std::unique_ptr<ISystem>`。**
- [x] `ECS-010` 确认 SystemHandle generation 语义未变。 **完成（审计保持）：System handle 的 owner/generation 语义未被本轮改动破坏。**
- [x] `ECS-011` 确认 topology/prerequisite tests 未变。 **完成（审计保持）：拓扑、prerequisite 与 phase 编译路径未改变。**
- [x] `ECS-012` 确认 SystemUpdateContext 未新增 service locator。 **完成（审计保持）：`SystemUpdateContext` 未引入 Runtime 或 Service Locator。**
- [x] `ECS-013` 保留 SceneServices 装配期语义。 **完成（审计保持）：`SceneServices` 仍只承担装配期类型化服务语义。**
- [x] `ECS-014` 创建 `ecs/serialization`。
- [x] `ECS-015` 创建 `ecs/scene_format`。 **完成：新增真实 target `lux::engine::ecs::scene_format`。**
- [x] `ECS-016` 移动 Section wire identifiers。 **完成：`EntitySectionId`、`PersistentEntityId`、内容 Blob 标识进入 ECS identity/scene_format。**
- [x] `ECS-017` 移动 ComponentRecord/SectionImage。 **完成：LXES Section Schema、Archetype、Column、Relocation、Attachment 与 Image 进入 ECS Scene Format。**
- [x] `ECS-018` 移动纯 Decoder/Encoder/Validation。 **完成：纯 Section 编码、解码、验证和 Persistence Journal 已由 ECS Scene Format 实现。**
- [x] `ECS-019` 确保 ECS format 不包含 ExtensionId。 **完成：边界扫描确认 ECS Scene Format 不包含 `ExtensionId`。**
- [x] `ECS-020` 确保 ECS format 不包含 SceneFeatureId。 **完成：边界扫描确认 ECS Scene Format 不包含 `SceneFeatureId`。**
- [x] `ECS-021` 创建 Engine ScenePackage。 **完成：新增 Engine-owned `engine/scene/package` 与 `ScenePackage`；公共 Codec API 现具备共享库 visibility，并由 standalone public-link probe 验证 import-library 边界。**
- [x] `ECS-022` 把 RequiredExtension 移到 Engine package。 **完成：`RequiredExtension` 位于 Engine Scene Package。**
- [x] `ECS-023` 把 FeatureRequest 移到 Engine package。 **完成：`SceneFeatureRequest` 使用 `SceneFeatureId` 并位于 Engine Scene Package。**
- [x] `ECS-024` 建立 legacy EntityScene split adapter。 **完成并退役：迁移期私有 adapter 曾隔离公共 Scene Package API；原生 LXSC v1 Codec 完成后 `LegacyEntitySceneAdapter` 已删除，未保留 shim 或 overload。**
- [x] `ECS-025` 建立 Entity Section Golden Files。 **完成：canonical ECS Scene Format Codec 对版本化 LXES v1 与 Persistence Journal v1 冻结 fixture 执行 decode/re-encode 逐字节契约，并覆盖截断、尾随字节、hash/digest 损坏和 limits。**
- [x] `ECS-026` 测试 unknown component policy。
- [x] `ECS-027` 测试 feature transaction 与 schedule commit。 **完成：`scene_contributions_test` 在当前 Windows x64 / MSVC / RelWithDebInfo 构建树运行通过，覆盖 cold assembly、transaction commit、失败 rollback、动态 enable/disable 与 close。**
- [ ] `ECS-028` 把 reflection pending static chain 改显式 draft。

### ECS Component Archive 二次裁决施工追踪

- [x] `ECSSER-001` 首笔提交只修订 Core Serialization / ECS Component Archive ADR 与事实文档。
- [x] `ECSSER-002` Core Serialization 删除 reflected tagged archive、Meta 与 Eigen 闭包。
- [x] `ECSSER-003` 建立 `lux::engine::ecs::component_archive` 与详细 expected 错误合同。
- [x] `ECSSER-004` 保持全部 tag ordinal 与包含 wire 的 Golden bytes 不变。
- [x] `ECSSER-005` tag 48 源码语义改为 UUID；资产引用只由显式 `asset_type=` annotation 表达。
- [x] `ECSSER-006` Authoring compatible field drift 与 Cooked exact schema policy 通过契约测试。
- [x] `ECSSER-007` Unknown Component schema 在 Authoring、Toolchain、Runtime 均明确拒绝且不发布部分 Registry 状态。
- [x] `ECSSER-008` Core/Component Archive/Function Animation installed consumer 闭包通过。
- [x] `ECSSER-009` 四 Profile 全量、owner tests、第二轮 no-op 与安装前缀同步通过。
## M4：Engine Execution 与公共 Asset 异步适配

- [ ] `ENGEA-001` 将 `engine/runtime/execution` 移到 `engine/execution`。
- [ ] `ENGEA-002` 创建 `Executor` façade。
- [ ] `ENGEA-003` 创建 `ExecutorBuilder`。
- [ ] `ENGEA-004` 将 AsyncScope 改为 TaskGroup。
- [ ] `ENGEA-005` 将 MainThreadMailbox 内收 detail。
- [ ] `ENGEA-006` 统一 Executor close task。
- [ ] `ENGEA-007` 保留 bounded typed operation queues。
- [ ] `ENGEA-008` 保留 operation dependency validation。
- [ ] `ENGEA-009` 不把 Executor 下沉 modules。
- [ ] `ENGEA-010` 将 `AssetManager` 与 `AssetLoadService` 合并到 Engine assets。 **SUPERSEDED：保持 Resource AssetManager + Runtime AssetLoadService 两层职责。**
- [ ] `ENGEA-011` 创建 AssetStore open/load/find/release API。 **SUPERSEDED：复用现有 AssetManager/AssetRef/AssetLoadService。**
- [ ] `ENGEA-012` 创建 AssetHandle generation。 **SUPERSEDED：不引入第二套 handle；保留 AssetRef 与 revision。**
- [ ] `ENGEA-013` 创建 AssetLease/immutable snapshot 规则。 **SUPERSEDED：不引入新 Lease 概念。**
- [ ] `ENGEA-014` 实现 AssetStore budget/eviction。 **SUPERSEDED：保留现有 AssetManager 驱逐行为，本轮不重写预算策略。**
- [ ] `ENGEA-015` 建立 SceneAssetResolver adapter。 **SUPERSEDED：SceneAsset 直接进入现有 Catalog/VFS/AssetLoadService。**
- [ ] `ENGEA-016` 移除 SystemUpdateContext 的 AssetStore 可能访问。 **SUPERSEDED：不存在 AssetStore；既有窄 Asset 服务边界另行维护。**
- [ ] `ENGEA-017` 测试 concurrent deduplicated load。 **SUPERSEDED：相同行为由现有 AssetLoadService 的 SceneAsset 集成测试覆盖。**
- [ ] `ENGEA-018` 测试 eviction stale handle。 **SUPERSEDED：不创建 generation handle；保留 AssetRef/revision 契约。**
- [ ] `ENGEA-019` 测试 close with accepted load。 **SUPERSEDED：继续属于现有 Runtime AssetLoadService 关闭契约。**
- [ ] `ENGEA-020` 删除 runtime_assets 旧 target。 **SUPERSEDED：`engine/runtime/assets` 是保留的异步编排适配器。**
## M4：Engine Extensions

- [x] `EXT-001` 创建 `engine/extensions/api`。 **完成：`engine/extensions/api` target 已建立。**
- [ ] `EXT-002` 创建 `engine/extensions/loader`。
- [ ] `EXT-003` 创建 `engine/extensions/registration`。
- [ ] `EXT-004` 创建 `engine/extensions/reflection`。 **后续 ABI/codegen 裁决：本轮保留已事务化的 reflection draft，不暗中增加 v4 plugin entry。**
- [ ] `EXT-005` 将 ExtensionModuleManager 改为 ExtensionLoader。
- [ ] `EXT-006` 将 ModuleLifetime/ModuleLease 改为 ExtensionLease。 **SUPERSEDED FOR ABI v4：ABI-facing registrar 可见布局不改名；纯宿主内部整理另立波次。**
- [x] `EXT-007` 移动 Extension ABI。 **完成：Engine API 直接拥有 v4 surface，Core owner 与 forwarding 依赖删除。**
- [x] `EXT-008` 建立 Extension Descriptor version tests。 **完成：已有 ABI version/layout/symbol compatibility tests。**
- [ ] `EXT-009` 将 RuntimeContributionRegistrar 改为 ExtensionRegistrar。 **SUPERSEDED FOR ABI v4：类型名和 inline/object layout 属于现行 plugin surface；新命名只能随新 ABI。**
- [ ] `EXT-010` 将 RuntimeRegistrationDraft 改为 ExtensionDraft。 **SUPERSEDED FOR ABI v4：不在 owner 搬迁中改变 registrar 可见类型。**
- [ ] `EXT-011` Registrar 仅暴露 components/sceneFeatures/render/operations。
- [x] `EXT-012` 删除通用 ContributionId。 **完成：领域 ID 为唯一接口；v4 descriptor 布局未改变。**
- [ ] `EXT-013` 拆除 EngineExtensions 聚合对象。
- [ ] `EXT-014` 将 SceneContributions 迁为 Scene Feature Catalog。 **进行中：ID、Catalog 查询、动态命令和产品调用点已强制使用 `SceneFeatureId`；旧 `SceneContributions` 类型名与表面尚未全部改为 Feature。**
- [ ] `EXT-015` 将 RenderEffects 迁到 Render 领域 Catalog。 **进行中：`RenderEffectId`、Scene Feature dependency 类型、Catalog 校验与测试已领域化；Catalog/Host 实现仍位于 `engine/runtime/extensions/contribution_host`，尚未迁入 Render owner。**
- [x] `EXT-016` 保留 validate-before-publish。 **完成（保持）：Extension 注册继续采用 validate-before-publish。**
- [ ] `EXT-017` 保留 operation first install 顺序。
- [ ] `EXT-018` 实现 Catalog commit rollback。
- [ ] `EXT-019` 测试 missing dependency。
- [ ] `EXT-020` 测试 dependency cycle。
- [ ] `EXT-021` 测试 hash collision。
- [ ] `EXT-022` 测试 duplicate schema/feature/operation。
- [ ] `EXT-023` 测试 unload with live lease rejection。
- [ ] `EXT-024` 测试 unload after scene close。
- [x] `EXT-025` 删除 modules/core/extension_abi。 **完成：目录、target、component、include 与安装 export 全部删除，无兼容层。**
- [x] `EXT-026` 确认 Modules SDK 不安装 Extension API。 **完成：Core package 不导出 Extension；独立 `lux-engine-extensions/extension_api` installed consumer 通过。**

### Extension ABI v4 owner 与 Core 清零施工追踪

- [x] `EXTABI-001` 首笔提交只修订 Extension ABI v4 owner、冻结表面与 Core 清零 ADR。 **完成：文档裁决提交 `2a916295`。**
- [x] `EXTABI-002` Engine Extension API 直接拥有 ExtensionId、descriptor/version/result、函数类型与 symbol constants。 **完成：实施提交 `c56efbc4`。**
- [x] `EXTABI-003` 保持 ABI v4 namespace、layout、ordinal、fingerprint 和三个导出 symbol string 不变。 **完成：Windows x64 精确 layout 与动态 DLL 契约通过。**
- [x] `EXTABI-004` 删除通用 ContributionId 及全部 production/test/CMake 引用，不建立替代通用 ID。 **完成：精确词扫描归零。**
- [x] `EXTABI-005` 删除 `modules/core/extension_abi` 目录、target、component、旧 include 与安装导出，不留兼容层。 **完成。**
- [x] `EXTABI-006` Authoring 保持 source DTO，Toolchain/Editor 显式转换；Runtime、Scene、Game、Editor Extension 与 fixture 只消费 Engine Extension API。 **完成：避免 AUTHORING→RUNTIME 反向依赖。**
- [x] `EXTABI-007` Extension ABI layout/symbol、loader、rollback、dependency 与实际 Physics2D Extension owner tests 通过。 **完成：动态 transaction、Game Export extension smoke 与 Physics2D export inventory 通过。**
- [x] `EXTABI-008` installed Extension SDK consumer 可配置/链接，旧 Core component 查找失败。 **完成：六头 consumer 运行成功，负向配置明确拒绝 extension_abi。**
- [x] `EXTABI-009` Debug、RelWithDebInfo、Android 安装前缀中旧 Core ABI 头与 export 精确清零。 **完成：六个新头 SHA 与 source 一致。**
- [x] `EXTABI-010` 四 Profile 全量、第二轮 no-op、旧符号扫描与 `git diff --check` 通过。 **完成；CTest 四树执行但当前注册 0 项，owner executables 单独运行。**
## M4：Engine Scene/Spatial/Render Bridge

- [ ] `SCENE-001` 移动 SceneRuntime 到 engine/scene/Scene。 **SUPERSEDED：`engine/scene` 只拥有 Scene Asset 数据/SerDeser，Runtime 生命周期保持独立。**
- [ ] `SCENE-002` 创建 SceneDependencies 精确引用结构。
- [ ] `SCENE-003` 删除 Scene 构造中的通用 Runtime 参数。
- [ ] `SCENE-004` 将 SceneContribution 表面改为 Feature。 **进行中：Runtime ID 已 Feature 化，且旧 `ContributionIdView` 兼容重载已删除；旧 `SceneContribution*` 类名仍待迁移。**
- [ ] `SCENE-005` 创建 FeatureCatalog。 **进行中：现有 Catalog 已强制使用 `SceneFeatureId`，并有 identity contract test；正式 `FeatureCatalog` 命名/API 尚未完成。**
- [ ] `SCENE-006` 创建 Scene::Features owner。
- [ ] `SCENE-007` 把 transaction/batches 内收 detail。
- [ ] `SCENE-008` 将 SceneScriptRuntime 拆到 Script Feature/Session。
- [x] `SCENE-009` 把 runtime/entity_scene pure decoder 移到 ECS format。 **完成：纯 LXES Decoder/Encoder/Validation 由 `ecs/scene_format` 唯一实现；旧 Resource Codec 已删除，兼容性由 canonical Golden fixture 契约证明。**
- [ ] `SCENE-010` 把 Stager/Loader 移到 engine/scene/loading。 **SUPERSEDED：Scene 不拥有 IO/异步；LXES Runtime loading 继续由现有 Runtime owner 负责。**

## M4：Scene Asset 边界修订

- [x] `SCENEASSET-001` 将 `engine/scene/api` 与 `engine/scene/package` 收敛为单一 `scene` component/target。
- [x] `SCENEASSET-002` 将 `ScenePackage`/`ScenePackageId` 迁为 `SceneDescription`/`asset_id_t`，不留 alias 或旧 include。
- [x] `SCENEASSET-003` 建立 `SceneAsset : TAsset<SceneDescription>` 与 Engine-owned `kSceneAssetType` 兼容数值。
- [x] `SCENEASSET-004` 建立 public `SceneAssetSerDeser : TAssetSerDeser<std::monostate>`，不得在 SceneAsset 上复制静态 Codec。
- [x] `SCENEASSET-005` 使用 `0x0130914D` 标准 AssetFileHeader 包裹原样 LXSC v1 data，并校验 outer/inner ID。
- [x] `SCENEASSET-006` 支持历史裸 LXSC `0x4353584C` 只读加载，重新导出只写包裹格式。
- [x] `SCENEASSET-007` 扩展 AssetCodecDescriptor/Catalog 的主/legacy magic、`findByMagic()`、冲突校验和 legacy shell 回调。
- [x] `SCENEASSET-008` 让 AssetManager、AssetLoadService、Pak 与 Toolchain 统一经 Catalog magic 分派，删除中央 `assetTypeOfMagic()`。
- [x] `SCENEASSET-009` 让 Provider/Pak 暴露和接收原始 magic，保持 Pak v2 `asset_magic` wire 不变。
- [x] `SCENEASSET-010` 从 Resource 删除 LXSC/LXES magic、Scene enum 名称、boot Scene 规则和 Scene 特判。
- [x] `SCENEASSET-011` Game/Editor/Toolchain 以标准 descriptors + Scene descriptor 构建 immutable Catalog。
- [x] `SCENEASSET-012` GameExporter 输出包裹 SceneAsset，Section 继续输出裸 LXES 且路径不变。
- [x] `SCENEASSET-013` RuntimeLaunchManifest 强制显式 `boot_scene`，删除唯一 Scene 自动选择回退。
- [x] `SCENEASSET-014` Game 经 VFS/AssetLoadService 加载 SceneAsset；SceneRuntime 持有 typed AssetRef，不接收裸 LXSC。
- [x] `SCENEASSET-015` Editor 临时场景注册为 SceneAsset，且不借机重构 Editor 架构。
- [x] `SCENEASSET-016` TerrainTile 值/Codec/tests 二次归位 `ecs/terrain`，保持 LXTT v1 wire。
- [x] `SCENEASSET-017` TilemapChunk 值/Codec/tests 二次归位 `ecs/tilemap`，保持 LXTC v1 wire。
- [x] `SCENEASSET-018` StaticColliderBatch3D 值/Codec/tests 二次归位 `ecs/physics3d`，保持 LXPC v1 wire。
- [x] `SCENEASSET-019` 验证 Scene/Payload Golden、Catalog 冲突、异步加载、显式启动和安装 consumer 契约。
- [x] `SCENEASSET-020` 删除旧 Scene components/includes 与 Resource 场景 Payload 头，完成旧符号/安装树归零扫描。
- [ ] `SCENE-011` 把 StartupSectionSystem 移到 Scene Feature。
- [x] `SCENE-012` 把 World Partition 配置移到 engine/spatial3d。 **完成：`engine/spatial3d` 拥有 Scene Catalog、L3SC v1 codec、Demand Channel 与 Entity Section canonical ID；Runtime、Game、Toolchain 与测试消费者已迁移，legacy `resource/spatial3d_scene` 已删除。**
- [ ] `SCENE-013` 把 runtime packs 按 Scene Feature 或 Game recipe 归位。
- [ ] `SCENE-014` 移动 runtime/render/scene 到 scene_bridge。
- [ ] `SCENE-015` 删除 EditorRenderInfra。
- [ ] `SCENE-016` 确保 Scene Bridge 不暴露 Renderer channels。
- [ ] `SCENE-017` 测试 headless Scene。
- [ ] `SCENE-018` 测试 rendered Scene。
- [ ] `SCENE-019` 测试 feature enable/disable rollback。
- [ ] `SCENE-020` 测试 close while section loading。
- [ ] `SCENE-021` 测试 world partition capacity。
## M5：Game 与 Editor 产品

- [ ] `PRODUCT-001` 创建 `lux::game::Game` façade。
- [ ] `PRODUCT-002` 创建 `game::Session`。
- [ ] `PRODUCT-003` 让 GameApplication 内部先调用 Session。
- [ ] `PRODUCT-004` 让 Editor Play 调用同一 Session。
- [x] `PRODUCT-005` 创建 GameManifest 新路径。 **完成：Game Launch Manifest 已迁到 `engine/game/deployment`。**
- [ ] `PRODUCT-006` 将 `engine_pak` 改为 `basePack` 外观。
- [ ] `PRODUCT-007` 保持旧 manifest reader。
- [ ] `PRODUCT-008` 移动 game_application 到 engine/game。
- [ ] `PRODUCT-009` 移动 player main 到 products/player。
- [ ] `PRODUCT-010` 删除 GameHost 类。
- [ ] `PRODUCT-011` 让 desktop main 直接驱动 Game。
- [ ] `PRODUCT-012` 让 Android entry 直接驱动 Game。
- [ ] `PRODUCT-013` 创建 `lux::editor::Editor` façade。
- [ ] `PRODUCT-014` 移动 editor product entry 到 products/editor。
- [ ] `PRODUCT-015` 删除 engine/hosts 目录。
- [ ] `PRODUCT-016` Game 不依赖 GLFW。
- [ ] `PRODUCT-017` Game 不依赖 ImGui。
- [ ] `PRODUCT-018` Editor 不依赖 player executable。
- [ ] `PRODUCT-019` 导出游戏可执行文件使用项目名。
- [ ] `PRODUCT-020` 导出 inventory 拒绝 Editor/Toolchain/Authoring。
- [ ] `PRODUCT-021` 测试 Edit→Play→Edit Session 一致性。
- [ ] `PRODUCT-022` 测试 Game/Editor close 顺序。
## M6：Workspace/Content/Documents

- [ ] `WORKSPACE-001` 创建 Workspace。
- [ ] `WORKSPACE-002` 把 ProjectController 状态迁入 Workspace。
- [ ] `WORKSPACE-003` 创建 ContentIndex。
- [ ] `WORKSPACE-004` 迁移 AssetRegistry 数据到 ContentIndex。
- [ ] `WORKSPACE-005` 创建 Content。
- [ ] `WORKSPACE-006` 迁移 import 业务到 Content。
- [ ] `WORKSPACE-007` 迁移 create 业务到 Content。
- [ ] `WORKSPACE-008` 迁移 delete 业务到 Content。
- [ ] `WORKSPACE-009` 迁移 move/rename 业务到 Content。
- [ ] `WORKSPACE-010` 将 AssetFileWatcher 改为 ContentWatcher。
- [ ] `WORKSPACE-011` 确保 Workspace 不 include UI。
- [ ] `WORKSPACE-012` 创建 Documents owner。
- [ ] `WORKSPACE-013` 创建 DocumentId/DocumentHandle。
- [ ] `WORKSPACE-014` 创建 SceneDocument。
- [ ] `WORKSPACE-015` 创建 MaterialDocument。
- [ ] `WORKSPACE-016` 创建 MaterialInstanceDocument。
- [ ] `WORKSPACE-017` 创建 FlowDocument。
- [ ] `WORKSPACE-018` 创建 ScriptDocument。
- [ ] `WORKSPACE-019` 建立 dirty/save/close 统一语义。
- [ ] `WORKSPACE-020` 建立 unsaved confirmation 由 Workbench 处理。
- [ ] `WORKSPACE-021` 把 Scene Cook 移到 Toolchain。
- [ ] `WORKSPACE-022` 测试 project open/close/switch。
- [ ] `WORKSPACE-023` 测试 content operations。
- [ ] `WORKSPACE-024` 测试 document dirty/save/close。
## M6：Workbench/Panels

- [ ] `WORKBENCH-001` 创建 Workbench。
- [ ] `WORKBENCH-002` 把 EditorShell 改为 Workbench。
- [ ] `WORKBENCH-003` 把 EditorMenuBar 改为 MainMenu。
- [ ] `WORKBENCH-004` 合并或删除 EditorToolHost。
- [ ] `WORKBENCH-005` 删除 EditorTools façade。
- [ ] `WORKBENCH-006` 将 EditorPanelCatalog 改为 PanelCatalog。
- [ ] `WORKBENCH-007` 限制 PanelContext 只用于 factory。
- [ ] `WORKBENCH-008` Descriptor 声明 required services。
- [ ] `WORKBENCH-009` Panel 构造后不保存 PanelContext。
- [ ] `WORKBENCH-010` 移除 Panel 对 Editor& 的依赖。
- [ ] `WORKBENCH-011` 移除 setAssetServices。
- [ ] `WORKBENCH-012` 移除 setPreviewHost。
- [ ] `WORKBENCH-013` 移除 setCompileDispatch。
- [ ] `WORKBENCH-014` 移除 setPrecompileHook。
- [ ] `WORKBENCH-015` 移除 create/delete/activate handlers。
- [ ] `WORKBENCH-016` 将 AssetBrowser 改为 ContentBrowser。
- [ ] `WORKBENCH-017` 将 SceneFeatureSettingPanel 改为 SceneSettings。
- [ ] `WORKBENCH-018` 将 ExtensionMonitorPanel 改为 ExtensionsView。
- [ ] `WORKBENCH-019` 将 `UISystem` 改为 `UI`。
- [ ] `WORKBENCH-020` 测试 Panel open/close/active handle。
- [ ] `WORKBENCH-021` 测试 extension panel create missing dependency。
- [ ] `WORKBENCH-022` 测试 Workbench close before async completions。
## M6：Flow/Material/Script/Preview

- [ ] `TOOLS-001` 创建 FlowCommands。
- [ ] `TOOLS-002` 创建 FlowCompiler。
- [x] `TOOLS-003` Flow compile request 拥有 graph snapshot。 **完成：Flow compile job 拥有 graph 与 cache/output 输入快照。**
- [ ] `TOOLS-004` 删除 FlowGraphPanel asset services。
- [x] `TOOLS-005` 删除 FlowGraphPanel precompile hook。 **完成：FlowGraph 预编译不再通过 Panel hook，改为显式 Compiler 依赖。**
- [ ] `TOOLS-006` 删除 NodeRegistry::global。
- [ ] `TOOLS-007` 创建 FlowNodeCatalog。
- [x] `TOOLS-008` 将 FlowGraphView 必需指针改引用。 **完成：`FlowGraphView`/`FlowSchema` 的必需依赖改为引用。**
- [ ] `TOOLS-009` 创建 MaterialCommands。
- [ ] `TOOLS-010` 创建 MaterialCompiler。
- [ ] `TOOLS-011` Material compile request 带 DocumentRevision。
- [ ] `TOOLS-012` Material latest-wins 移出 Panel。
- [ ] `TOOLS-013` 拆 Material 与 MaterialInstance documents。
- [ ] `TOOLS-014` MaterialPreview 只消费 effective snapshot。
- [ ] `TOOLS-015` 将 MaterialGraphView 必需指针改引用。
- [ ] `TOOLS-016` 创建 ScriptCommands。
- [ ] `TOOLS-017` 区分 Editor tooling ScriptRuntime 与 Game Session runtime。
- [ ] `TOOLS-018` 创建 PreviewScene。
- [ ] `TOOLS-019` MaterialPreviewHost 改为 MaterialPreview。
- [ ] `TOOLS-020` ThumbnailService 使用 PreviewScene。
- [ ] `TOOLS-021` 删除 Preview 私有 RuntimeHost。
- [ ] `TOOLS-022` 统一 preview open/update/close。
- [ ] `TOOLS-023` 测试 Flow close during compile。
- [ ] `TOOLS-024` 测试 Material close during compile。
- [ ] `TOOLS-025` 测试 Preview + Play coexist。
- [ ] `TOOLS-026` 测试 thumbnail invalidation。
## M7：CMake/Namespace/Compatibility

- [ ] `BUILD-001` 创建新领域真实 targets。
- [ ] `BUILD-002` 新目标使用 `lux::<domain>` aliases。
- [ ] `BUILD-003` 旧 targets 仅作为 aliases。
- [ ] `BUILD-004` 生成 forwarding headers。
- [ ] `BUILD-005` 迁移仓内 includes 到新 prefix。
- [ ] `BUILD-006` 迁移仓内 target_link_libraries 到新 target。
- [ ] `BUILD-007` 创建 lux-core package。
- [ ] `BUILD-008` 创建 lux-platform package。
- [ ] `BUILD-009` 创建 lux-resource package。
- [ ] `BUILD-010` 创建 lux-render package。
- [ ] `BUILD-011` 创建 lux-input package。
- [ ] `BUILD-012` 创建 lux-animation package。
- [ ] `BUILD-013` 创建 lux-navigation package。
- [ ] `BUILD-014` 创建 lux-script package。
- [ ] `BUILD-015` 创建 lux-ui package。
- [ ] `BUILD-016` 创建 lux-engine-extension-sdk package。
- [ ] `BUILD-017` 拆 Runtime library package。
- [ ] `BUILD-018` 分离 Build Tool packages。
- [ ] `BUILD-019` 更新 CPack components。
- [ ] `BUILD-020` 检查 relocatable configs。
- [ ] `BUILD-021` 检查 cross compile host tools。
- [ ] `BUILD-022` 运行 rewrite_includes 脚本并审阅未确定项。
- [ ] `BUILD-023` 运行 legacy symbol report。
- [ ] `BUILD-024` 删除旧 package configs。
- [ ] `BUILD-025` 删除 forwarding headers。
- [ ] `BUILD-026` 删除旧 aliases。
## 测试与发布

- [ ] `TEST-001` 采集重构前基线。
- [ ] `TEST-002` 运行 Modules SDK installed samples。
- [ ] `TEST-003` 运行 ECS SDK sample。
- [ ] `TEST-004` 运行 Extension SDK fixture。
- [x] `TEST-005` 运行 PLAYER Profile。 **完成：Windows x64 / MSVC / RelWithDebInfo 全量 `target all`、CTest 与最终 no-op 构建通过。**
- [x] `TEST-006` 运行 EDITOR Profile。 **完成：Windows x64 / MSVC / RelWithDebInfo 全量 `target all`、受影响 Editor roundtrip tests 与最终 no-op 构建通过。**
- [x] `TEST-007` 运行 TOOLCHAIN Profile。 **完成：Windows x64 / MSVC / RelWithDebInfo 全量 `target all`、Spatial3D cook/game export tests 与最终 no-op 构建通过。**
- [x] `TEST-008` 运行 DEVELOPER Profile。 **完成：Windows x64 / MSVC / RelWithDebInfo 全量 `target all`、完整 CTest/owner contract tests 与最终 no-op 构建通过。**
- [x] `TEST-009` 运行 Windows x64。 **完成：以 `b1a25d3bb23f33f092964465c7d27d819beaf7db` 为基线的当前施工工作树已完成 Windows x64 / MSVC / RelWithDebInfo 四 Profile 全量构建与测试。**
- [ ] `TEST-010` 运行 Linux x64。
- [ ] `TEST-011` 运行 Android arm64 配置/构建。
- [ ] `TEST-012` 运行 AddressSanitizer。 **部分完成：本轮 Scene Package/Format smoke 执行了 AddressSanitizer；尚未完成全项目矩阵。**
- [ ] `TEST-013` 运行 UndefinedBehaviorSanitizer。 **部分完成：本轮 Scene Package/Format smoke 执行了 UndefinedBehaviorSanitizer；尚未完成全项目矩阵。**
- [ ] `TEST-014` 运行 ThreadSanitizer。
- [ ] `TEST-015` 运行所有 legacy asset golden tests。
- [x] `TEST-016` 运行 ECS section/package golden tests。 **完成：LXSC v1、LXES v1 与 Persistence Journal v1 的版本化 frozen fixture 已由 canonical Codec 执行 decode/re-encode 逐字节验证，ScenePackage validation/public-link 与 EntitySection loading/wire owner tests 均通过。**
- [ ] `TEST-017` 运行 Game manifest golden tests。
- [x] `TEST-018` 运行 Extension ABI layout/symbol tests。 **完成：Extension ABI layout、version 与旧 symbol string tests 已建立。**
- [ ] `TEST-019` 运行 Script ABI tests。
- [ ] `TEST-020` 运行 failure injection matrix。
- [ ] `TEST-021` 运行 close ordering tests。
- [ ] `TEST-022` 运行 exporter inventory scan。
- [x] `TEST-023` 运行二次 no-op build。 **完成：所有受影响 Profile 的最终第二轮 Ninja 构建均为 `ninja: no work to do`。**
- [ ] `TEST-024` 比较二进制尺寸与启动/关闭基线。
- [ ] `TEST-025` 归档 architecture graph 和 test reports。
## 每个 Pull Request 提交前

- [x] `PR-001` PR 标题包含领域与边界，而不是“cleanup/refactor misc”。 **完成：当前 PR 标题明确 SDK boundary 与 Scene Format ownership。**
- [x] `PR-002` PR 描述列出当前问题。 **完成：PR 描述列出半完成迁移、Windows 宏冲突与 Resource 语义下沉问题。**
- [x] `PR-003` PR 描述列出目标所有者。 **完成：PR 描述明确 ECS Scene Format 与 Engine Scene Package 的目标所有者。**
- [x] `PR-004` PR 描述列出 CREATE/MOVE/SPLIT/MODIFY/DELETE。 **完成：PR 描述按新增 target、迁移模型、兼容适配与待删除 component 分类。**
- [ ] `PR-005` PR 描述附 target dependency before/after。
- [x] `PR-006` PR 描述说明 public API/ABI/file format 变化。 **完成：PR 明确本轮不改变 LXSC/LXES wire bytes，新增 API 为强类型边界。**
- [x] `PR-007` PR 描述说明 compatibility layer。 **完成：PR 明确旧 Resource DTO/Codec 为临时兼容层。**
- [x] `PR-008` PR 描述说明 compatibility 删除里程碑。 **完成：兼容删除条件为 Runtime/Authoring/Toolchain 消费者全部切换后删除 `modules/resource/entity_scene`。**
- [x] `PR-009` 确认无新 service locator。 **完成：本轮未新增 Service Locator。**
- [x] `PR-010` 确认无新必需依赖 setter。 **完成：本轮未新增必需依赖 setter/hook。**
- [x] `PR-011` 确认无新不必要 shared_ptr。 **完成：新 Scene Format/Package 为值模型，未以 `shared_ptr` 隐藏所有权。**
- [ ] `PR-012` 确认所有裸指针语义明确。
- [ ] `PR-013` 确认构造失败回滚。
- [ ] `PR-014` 确认 close 路径。
- [ ] `PR-015` 确认异步快照拥有输入。
- [ ] `PR-016` 确认 Extension Lease。
- [x] `PR-017` 确认 public headers 自包含。 **完成：identity、scene_format、scene_package 公共头均有 contract/smoke 编译覆盖；`ScenePackageCodec.hpp` 现显式包含其 generated visibility header。**
- [x] `PR-018` 确认 installed sample。 **完成：RelWithDebInfo 安装前缀的外部 canonical consumer 已成功配置并链接 ScenePackage/SceneCatalog；两个旧 Resource component 的 `find_package` 均按预期失败。**
- [x] `PR-019` 确认 target graph gate。 **完成：架构 target graph/boundary gate 持续启用。**
- [x] `PR-020` 确认 include gate。 **完成：下层 forbidden include/symbol 边界扫描通过。**
- [ ] `PR-021` 确认 legacy report 未增长。 **进行中：Scene Resource 旧 include/namespace/target 已全仓归零，并由 owner contract tests 与 CMake DAG 防回流；迁移期 Scene Feature Python gate 已按项目决定退役，全项目统一 legacy report 尚未完成。**
- [x] `PR-022` 确认受影响 tests。 **完成：受影响 contract、wire compatibility、ASAN/UBSAN smoke tests 已运行。**
- [x] `PR-023` 确认双构建 no-op。 **完成：CMake 变更后的连续构建已验证，最终第二轮无增量工作。**
- [x] `PR-024` 确认文档/映射表同步。 **完成：PR 描述、边界文档和本 Checklist 已同步。**
## 最终归零

- [x] `FINAL-001` 删除 `modules/core/extension_abi`。 **完成：源码、CMake、安装 component/export 与旧 include 全部归零。**
- [x] `FINAL-002` 删除 `modules/platform/common`。 **完成：目录、target、component、生成头、安装导出、旧 namespace/type 全部删除；不保留 alias、shim 或 forwarding header。**
- [ ] `FINAL-003` 删除 `modules/platform/gapi`。 **SUPERSEDED：ADR-20260821 确认 GAPI 永久保留。**
- [x] `FINAL-004` 删除 `modules/resource/deployment`。 **完成：`modules/resource/deployment` 已删除。**
- [x] `FINAL-005` 删除 `modules/resource/entity_scene`。 **完成：旧 target、component、源码、测试、公共头、namespace 与安装产物已删除；未保留 alias、shim、forwarding header 或测试专用旧 Codec。**
- [x] `FINAL-006` 删除 `modules/resource/spatial`。 **完成：旧源码、target、component、include prefix、namespace 与安装导出均已删除，不保留 alias/shim/forwarding header。**
- [x] `FINAL-007` 删除 `modules/resource/spatial3d_scene`。 **完成：Engine-owned `lux::spatial3d::SceneCatalog` 与 L3SC v1 Codec 为唯一 owner；旧 target、component、公共头、namespace 与安装产物已删除，不存在兼容 alias。**
- [x] `FINAL-008` 删除 `modules/resource/classic_mesh` 一级 component。 **完成：LXCB v1 值与 Codec 迁入 Function `render_standard_content`，旧 target/component/header/DLL/import library 全部退役，无 alias 或 shim。**
- [x] `FINAL-009` 删除 `modules/resource/terrain` 一级 component。 **完成：值类型迁入 `description`、LXTT v1 Codec 迁入 `asset`，旧安装与源码接口归零。**
- [x] `FINAL-010` 删除 `modules/resource/tilemap` 一级 component。 **完成：值类型迁入 `description`、LXTC v1 Codec 迁入 `asset`，旧安装与源码接口归零。**
- [x] `FINAL-011` 删除 `modules/resource/physics3d` 一级 component。 **完成：值类型迁入 `description`、LXPC v1 Codec 迁入 `asset`，旧安装与源码接口归零。**
- [ ] `FINAL-012` 删除 `engine/runtime` 目录。
- [ ] `FINAL-013` 删除 `engine/hosts` 目录。
- [ ] `FINAL-014` 删除 `GameHost`。
- [ ] `FINAL-015` 删除 `GameApplication` 旧 façade。
- [ ] `FINAL-016` 删除 `LuxEditor::Runtime`。
- [ ] `FINAL-017` 删除 `EditorAsyncService`。
- [ ] `FINAL-018` 删除 `EditorRenderInfra`。
- [ ] `FINAL-019` 删除 `EditorToolHost`。
- [ ] `FINAL-020` 删除 `EditorTools` façade。
- [ ] `FINAL-021` 删除 Controller 网络旧类型。
- [ ] `FINAL-022` 删除所有必需依赖 hooks。
- [ ] `FINAL-023` 删除 `NodeRegistry::global()`。
- [ ] `FINAL-024` 删除旧 include prefixes。
- [ ] `FINAL-025` 删除旧 CMake targets/packages。
- [x] `FINAL-026` 确认 modules/resource 仅有 description/asset。 **完成：物理目录、显式 CMake 子目录和安装组件三者一致。**
- [ ] `FINAL-027` 确认 Modules SDK target graph 无上层依赖。
- [ ] `FINAL-028` 确认 ECS Core 无 Engine 依赖。
- [ ] `FINAL-029` 确认导出游戏无 EngineRuntime/Editor 语义。
- [ ] `FINAL-030` 确认 12 映射表无 PENDING。

## 本轮施工记录（Scene Format / Scene Package）

- [x] 远端提交 `76c6a6555aeaa6ddcb598ba6960b5c6c25937046`：建立 `ecs/identity` 与 `ecs/scene_format`。
- [x] 远端提交 `bb39134c6e3edf00634c798e351eec18222b91bb`：建立 Engine-owned Scene Package，并补充 Engine-owned `ExtensionId.hpp` 窄入口。
- [x] 新旧 EntitySection 与 Persistence Journal 编码结果逐字节一致。
- [x] Canonical Scene Package 与旧 LXSC 编码结果逐字节一致。
- [x] 新 ECS Scene Format 边界扫描不包含 Extension、Scene Feature、AssetStore、Renderer、Editor 或 Engine Runtime。
- [x] 本地 C++23 smoke、AddressSanitizer、UndefinedBehaviorSanitizer 通过。
- [x] 维护者已确认前一闭环 Head `33e64b4b55707b48c86b5a371d59ad52c6a2cdd7` Windows 全量构建通过。
- [x] 维护者已在 Head `326ab0c28f3202db189745e1aae0740ae2d1049a` 完成 Windows x64 / MSVC / RelWithDebInfo 全量构建。
- [x] 迁移 Runtime、Authoring、Toolchain 消费者到 canonical ECS Scene Format / Engine ScenePackage / Authoring World API。
- [x] 删除 legacy `modules/resource/entity_scene` component；本仓未建立 forwarding alias，安装前缀亦已清理。


## 本轮施工记录（SceneFeature 编译修复 / 身份边界收紧）

- [x] 将 `platformer2d_visual_demo`、`rigidbody2d_visual_demo`、`tilemap_visual_stress`、`pixel_world_visual_stress` 的选中项改为 `SceneFeatureIdView`。
- [x] 将 Pixel World 失败诊断从 `failure.contribution` 迁为 `failure.feature`，并修复格式化参数错位。
- [x] 删除 `SceneContributionCatalog`、`SceneContributions`、Scene Feature owner 对 `ContributionIdView` 的兼容重载。
- [x] 将 `EntitySceneCatalog::findContribution()` 查询参数改为 `SceneFeatureIdView`，并显式链接 `scene_api`。
- [x] 新增 `entity_scene_catalog_identity_test`，编译期拒绝旧 `ContributionIdView` 查询。
- [x] 历史上曾以 `check_scene_feature_identity.py` / `scene-feature-identity` Job 冻结迁移边界；**本波次按项目决定删除，改由 Scene/Extension owner 编译、链接与契约测试负责。**
- [x] 维护者已对 Head `326ab0c28f3202db189745e1aae0740ae2d1049a` 完成 Windows x64 / MSVC / RelWithDebInfo 全量构建。
- [x] 将剩余 Runtime、Authoring、Toolchain 消费者迁到 `ecs/scene_format` / Engine `ScenePackage`。


## 本轮施工记录（Render Effect → Scene Feature dependency）

- [x] 将 `RenderEffectDescriptor::required_scene_features` 从 `std::vector<ContributionId>` 改为 `std::vector<SceneFeatureId>`。
- [x] 将 `dependenciesReady()` 的谓词参数显式限制为 `const SceneFeatureId&`，消除泛型 lambda 对错误身份类型的掩盖。
- [x] 将 Render Effect 公共头从旧 `core/extension_abi/StableId.hpp` 切换到 Engine-owned `ExtensionId.hpp` 和 `SceneFeatureId.hpp`。
- [x] 为 required Scene Feature dependency 增加非法 canonical name、重复 ID 与哈希冲突校验。
- [x] 扩展 `render_effect_catalog_test`，覆盖强类型、合法依赖、非法依赖和重复依赖。
- [x] 历史 `check_scene_feature_identity.py` 曾禁止 legacy wire 之外出现 `std::vector<lux::extensions::ContributionId>`；**该脚本已由项目决定退役，相同类型约束现由 owner 编译期断言与 contract tests 承担。**
- [x] Render Effect Scene Feature dependency C++20 smoke test通过。
- [x] GitHub Actions 补丁应用、source snapshot 与 Scene Feature identity Job 通过。
- [x] 维护者已对 Head `326ab0c28f3202db189745e1aae0740ae2d1049a` 完成 Windows x64 / MSVC / RelWithDebInfo 全量构建。
- [ ] 将 Render Effect Catalog/Host 从 Runtime Extension 聚合目录迁入最终 Render/Scene Bridge owner。


## 本轮施工记录（Scene Package DLL ABI / import library）

- [x] 定位 `LNK1104` 根因：`scene_package` 是共享组件，但公共 Codec 函数没有导出符号，Windows 未生成 `lux_engine_scene_package.lib`。
- [x] 在 `engine/scene/package/CMakeLists.txt` 增加 `generate_visibility_header()`。
- [x] 为 `scene_package` 编译目标定义 `LUX_ENGINE_SCENE_PACKAGE_LIBRARY`。
- [x] 在 `ScenePackageCodec.hpp` 显式包含 `lux/engine/scene/package/visibility.h`。
- [x] 使用 `LUX_ENGINE_SCENE_PACKAGE_PUBLIC` 导出 `validateScenePackage()`、`encodeScenePackage()`、`decodeScenePackage()`。
- [x] 新增 `scene_package_public_link_test`，仅链接 `lux::engine::scene::scene_package`，不直接链接 legacy Resource component。
- [x] 保留 `scene_package_contract_test` 作为 canonical ScenePackage 与旧 LXSC 的字节兼容测试，并在 CMake 中明确两类测试职责。
- [x] GitHub Actions `architecture-recovery` run 67 在 Head `326ab0c28f3202db189745e1aae0740ae2d1049a` 通过。
- [x] 维护者已在 Windows x64 / MSVC / RelWithDebInfo 上确认 `lib/lux_engine_scene_package.lib` 生成。
- [x] 维护者已确认 `scene_package_public_link_test.exe` 与 `scene_package_contract_test.exe` 均链接并运行通过。
- [x] Runtime、Authoring、Toolchain 的 legacy `entity_scene` 消费者已迁移到 canonical owner，并完成 Windows 四 Profile 验证。

## 本轮施工记录（运行时 Persistent Entity 身份归 ECS）

- [x] `PersistentEntityIdComponent` 与 `PersistentEntityIndex` 统一使用 `lux::ecs::PersistentEntityId`。
- [x] Pixel、Tilemap、Terrain、Visual LOD 与相关 Runtime System 统一使用 `lux::ecs::PersistentEntityRef`。
- [x] Entity Section staging/materialization 在 wire → runtime 边界显式按 UUID value 转换，未改变 LXES 字节格式。
- [x] `WorldActorEcsAdapter` 建立 `toAuthoringId()` / `toRuntimeId()`，Authoring DTO 与 ECS Registry 不再隐式共享类型。
- [x] `ecs/core` 删除 `lux::engine::resource::entity_scene` PUBLIC 依赖及安装期传递依赖。
- [x] wire compatibility test 断言 legacy ID 与 ECS ID 不可隐式转换，并验证底层 UUID 值一致。
- [x] 新增 `doc/ecs-persistent-identity-boundary.zh-CN.md`。
- [x] 历史上曾使用 `check_persistent_entity_identity.py` / `persistent-entity-identity` Job 辅助迁移；**本波次按项目决定删除，强类型隔离由 `WorldActorEcsAdapter` 的显式转换、编译期断言与 owner tests 验证；`source-snapshot` 保留。**
- [x] 已在以 `b1a25d3bb23f33f092964465c7d27d819beaf7db` 为基线的当前施工工作树执行 Windows x64 / MSVC / RelWithDebInfo 四 Profile 全量构建。
- [x] Runtime 的纯 EntitySection decode/stage/materialize 公共表面切换到 `ecs/scene_format`。
- [x] Toolchain `EntitySectionImageBuilder` / Cooker 切换到 ECS Scene Format，Manifest 切换到 Engine Scene Package。
- [x] 删除 legacy `modules/resource/entity_scene` component，不保留 forwarding aliases。


## 本轮施工记录（Canonical EntitySection staging 编译闭环）

- [x] 修复 `EntityBatchStager.cpp` 将 canonical `ComponentSchemaId{hash,name}` 误作旧 `StableNameId` 调用 `name()` / `hash()` 的 Microsoft Visual C++ 编译错误。
- [x] 修复 `EntityBatchMaterializer.cpp` 调用已删除 `toRuntimePersistentId()` 的迁移残留；canonical `persistent_id` 直接作为 ECS-owned `PersistentEntityId` 使用。
- [x] `EntityBatchStager` 将必需 `ComponentTypeCatalog` 从可空裸指针改为构造期引用，删除无效的运行时空检查。
- [x] `PreparedEntityBatchImpl` 在构造时取得 `SectionBlobStore&`，删除先置空、后补接的两阶段初始化。
- [x] `entity_section_public_contract_test` 增加 Stager 构造不变量及 canonical schema 字段模型的编译期断言。
- [x] 历史 `check_entity_section_boundary.py` / `entity-section-boundary` Job 曾用于迁移期紧缩依赖；**本波次按项目决定删除，不再记为当前门禁；EntitySection owner contract/public-link/loading tests 与 CMake DAG 继续覆盖边界。**
- [x] Canonical EntitySection staging 变更已包含在当前施工工作树的 Windows x64 / MSVC / RelWithDebInfo 四 Profile 全量构建中并通过。
- [ ] `SCENE-010`：把 Stager/Loader 的目录与公开命名迁入 `engine/scene/loading`；本轮仅收紧对象不变量，不提前勾选。
- [x] 删除 legacy `modules/resource/entity_scene` component 与 forwarding aliases；未保留 alias、shim 或测试专用旧 Codec。


## 本轮施工记录（Wire compatibility test 依赖所有权）

- [x] 定位静态断言失败的真实根因：测试未声明 `ecs/core`，却包含 `PersistentEntityIdComponent.hpp`，Microsoft Visual C++ 从安装树解析到旧版头文件。
- [x] 从 `entity_section_wire_compatibility_test` 移除运行时 ECS Component include 与 API 断言；该测试仅保留新旧 wire ID 隔离、UUID value 与 LXES/Persistence Journal 字节兼容。
- [x] 将 `PersistentEntityIdComponent::id()` 返回类型契约迁到 `ecs/core/test/persistent_entity_index_test.cpp`，由实际 owner target 编译验证。
- [x] 保持 wire compatibility target 不链接 `ecs/core`；没有通过扩大依赖闭包掩盖测试错误。
- [x] 历史 `check_entity_section_boundary.py` 曾检查 wire/loading test 依赖所有权；**该脚本已退役，契约现由 `scene_format_contract_test`、`entity_section_wire_compatibility_test`、`entity_section_public_contract_test` 与 loading integration tests 分 owner 验证。**
- [x] GitHub Actions `architecture-recovery` run 109 在 Head `97cdd030f9178dd0dd85d1e5ae54221de3ecee4b` 全部通过。
- [x] 当前施工工作树的 `entity_section_wire_compatibility_test` 已在 Windows x64 / Microsoft Visual C++ / RelWithDebInfo 下编译并运行通过。

## 本轮施工记录（ScenePackage 原生验证）

- [x] 新增 `ScenePackageValidation.cpp`，直接验证 Engine-owned `ScenePackage` / `SectionRecord`，不 include 或构造 `lux::entity_scene` DTO。
- [x] Stored Section path 复用 Asset `VirtualPath::parse()`，未在 Scene 领域复制路径 grammar。
- [x] 原生验证覆盖 Extension、Component Schema、Scene Feature、Demand Channel、Generator、canonical ordering、重复项、startup reference、Section dependency 与依赖环。
- [x] `encodeScenePackage()` 在转换到 legacy wire DTO 前执行 canonical validation；`decodeScenePackage()` 在 legacy decode 与模型转换后再次执行 canonical validation。
- [x] 新增 `scene_package_validation_test`，仅链接 Engine `scene_package`，覆盖合法模型、非法路径、重复 Feature、缺失 startup Section 与依赖环。
- [x] `scene_package_contract_test` 收敛为 LXSC v1 新旧编码逐字节兼容与交叉解码测试。
- [x] ScenePackage validation/contract/public-link tests 确保验证、Codec 与公共链接边界均归 canonical owner；legacy Adapter/Resource DTO 已删除，不再需要 Python 边界扫描例外。
- [x] 本地 C++20 syntax probe 与全部现有架构扫描通过。
- [x] Windows x64 / MSVC / RelWithDebInfo 的全量构建与 `scene_package_validation_test` 运行确认。
- [x] 原生 LXSC v1 encoder/decoder 直接面向 `lux::scene::ScenePackage`；legacy adapter 与 Resource PRIVATE link 已删除，`FINAL-005` 已勾选。

## 本轮施工记录（Spatial3D Scene Catalog 所有权与编译闭环）

- [x] 定位当前首错：legacy `lux::entity_scene::DemandChannelId` 无法写入 `std::vector<lux::scene::DemandChannelId>`；后续 Section、band aggregate 与 `sameStableId` 诊断均为同一跨领域 ID 泄漏的级联错误。
- [x] 新建 `engine/spatial3d` 真实 target，规范模型为 `lux::spatial3d::SceneCatalog` / `SceneCatalogBand` / `SceneCatalogEntry`。
- [x] Scene Catalog 的 Demand Channel 使用 `lux::scene::DemandChannelId`；Section 使用 `lux::ecs::scene_format::EntitySectionId`。
- [x] `Spatial3DPartitionedContribution` 从解码开始只处理 canonical ID，删除 production path 的 legacy `sameStableId` 跨 Tag 比较。
- [x] Runtime catalog lookup、interest band、section catalog entry 与 descriptor schema version 全部使用 Engine-owned contract。
- [x] GameApplication 使用 Engine Spatial3D Feature 常量，不再从 Resource 组件读取 Feature 名称。
- [x] Toolchain `Spatial3DEntitySceneAdapter` 的公开输入/输出迁移到 canonical Scene Catalog，production target 不再链接 legacy `spatial3d_scene`。
- [x] 实现 Engine-owned L3SC v1 encoder/decoder，并保持 Magic、Version、字段顺序、stable names、UUID 与数值 wire 表达不变。
- [x] 新增 `spatial3d_scene_catalog_contract_test`，逐字节比较新旧编码并执行双向交叉解码。
- [x] 新增 `spatial3d_scene_catalog_public_link_test`，仅链接新 target，验证动态库导出、import library 与安装依赖闭包。
- [x] 新增 `doc/spatial3d-scene-catalog-boundary.zh-CN.md`。
- [x] 历史 `check_spatial3d_catalog_boundary.py` / `spatial3d-catalog-boundary` Job 曾用于迁移期防回流；**本波次按项目决定删除，当前边界由 Scene Catalog contract/public-link/runtime/Toolchain tests 和 CMake DAG 负责。**
- [x] 以 `b1a25d3bb23f33f092964465c7d27d819beaf7db` 为施工基线，在 `codex/scene-resource-retirement` 完成 Windows x64 / MSVC / RelWithDebInfo 的 DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 构建。
- [x] 删除 `modules/resource/spatial3d_scene` 安装组件和旧 include prefix，并通过安装 consumer/legacy component 反向查找契约。

## 本轮施工记录（Scene Resource 兼容层清零）

- [x] `engine/scene/package` 建立 bounded 原生 LXSC v1 Codec，删除 `LegacyEntitySceneAdapter` 与对旧 Resource target 的 PRIVATE link。
- [x] `engine/spatial3d` 使用 L3SC v1 frozen fixture 验证旧字节 decode/re-encode，并覆盖截断、非法 ID、重复 band/entry、尾随字节与 limits。
- [x] Authoring 建立不互相隐式转换的 `WorldId` / `WorldActorId`，以及 Authoring-owned Feature/Extension DTO；Toolchain leaf 执行唯一显式 cooked 转换。
- [x] LXWA v4、LXAI v2、LXAD v2、LXIP v2、LXTP v1、LXTL v1、LXPP v1 的确定性 fixture 固定字节长度与 SHA-256，并逐字节验证 decode/re-encode。
- [x] LXES v1 与 Persistence Journal v1 改为 canonical Codec 对 frozen fixture 的契约，删除旧 EntityScene DTO/Codec/component。
- [x] RelWithDebInfo 安装前缀已重装并删除旧头、component export、DLL 与 import library；Debug 与 Android 前缀确认不存在旧 include prefix。
- [x] canonical 外部 consumer 可配置和链接；`find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene)` 与 `spatial3d_scene` 均按预期失败。
- [x] 项目决定删除 `check_scene_feature_identity.py`、`check_persistent_entity_identity.py`、`check_entity_section_boundary.py`、`check_spatial3d_catalog_boundary.py` 及对应 CI Job；改由 owner contract tests 与现有 CMake DAG 门禁承担边界。
- [x] `CORE-006` / `EXT-012` 后续结论：通用 `ContributionId` 不属于 ABI v4 descriptor 布局，已由 `c56efbc4` 删除；ABI-facing layout 与 symbol 未改变。

## 本轮施工记录（Spatial 基础值归 Math 与 Resource 最终清零）

- [x] 以 `fb8c4f3902444476bde65412e4dff81a6ca50971` 为基线建立 `codex/resource-spatial-retirement`，未推进 M0、AssetStore、Registry、`SCENE-010` 或全局 M7 package rename。
- [x] Core Math 新建 `Position.hpp`、`Grid.hpp`、`RelativePosition.hpp`，使用 `lux::math::Position2d` / `Position3d` / GridCoord；四种值类型的 16/24 字节布局、字段偏移、standard-layout 与 trivially-copyable 契约已固定。
- [x] ECS Core 建立非侵入 `LUX_REFLECT_EXTERNAL` adapter 与 traits，canonical full name 为 `lux::math::*`；Transform 嵌套 `RefClass` 与 Lua sidecar 均由 codegen 构建验证，旧反射全名不存在。
- [x] FlowGraph v3 新 Math Field Node 可 encode/decode/re-encode，历史 Resource 类名明确拒绝；未建立 legacy resolver，也未升级格式版本。
- [x] LXWA v4 全 wire family、World Descriptor Index v5、LXES v1、Persistence Journal v1 与 L3SC v1 owner tests 全部通过，既有固定长度/SHA 与 decode/re-encode 契约保持不变。
- [x] 删除 `modules/resource/spatial` 的源码、target、component、旧头与安装导出；Resource 根改为显式 `description` + `asset`，不保留 alias、shim 或 forwarding header。
- [x] RelWithDebInfo 安装完成，49 个受影响公共头同步至 Debug/RelWithDebInfo/Android；canonical Core Math 外部 consumer 配置、链接和运行通过，旧 Resource spatial component 查找按预期失败。
- [x] Windows x64 / MSVC / RelWithDebInfo 的 DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 全量 `target all -j 4 -k 0` 均通过，各构建树第二轮均为 `ninja: no work to do`；36 项 owner/wire/roundtrip/export 契约通过。
- [x] 本轮源码与 CMake 变更已形成独立 checkpoint commit `ad53a523302210a3fdd89e1fdd6a6447139470a0`；提交后工作树干净。
- [x] `SDK-003` 在后续 Asset Pipeline/Core Meta 波次完成；全局 M7 target/include prefix 迁移及该轮其他未授权项目仍保持未完成。

## 本轮施工记录（Resource Asset 目录与组件归一化）

- [x] 以当前 Platform Common checkpoint `bdcb653d2e7592d65aecc09e56855b0d0bbe9e49` 为基线建立 `codex/resource-asset-layout-normalization`；未从裸历史提交重新施工，未推进 M0、AssetStore、Scene、Registry、Editor 或全局 M7 包名迁移。
- [x] 将 `modules/resource/asset` 的 `core/src`、`codecs/src`、`pak/src`、Codec tests 和私有头迁入统一 `include/sinclude/pinclude/src` 结构；旧 `core`、`codecs`、`identity`、`pak` 顶层功能目录与独立 CMakeLists 已删除，无 forwarding header。
- [x] 四个旧 Asset target 合并为唯一 `lux::engine::resource::asset` / `lux_engine_asset`；旧 target、alias、export、安装 component 与 visibility 宏已删除，公共 Codec/Pak 头分别归入 `include/.../asset/codecs`、`include/.../asset/pak`。
- [x] Asset visibility 收敛为 `lux/engine/resource/asset/visibility.h` 与 `LUX_ASSET_PUBLIC`；消费者、测试、Toolchain、Runtime、ECS、Authoring、Editor 的链接和 include 已统一到 canonical owner。
- [x] 全仓模块布局审计确认：独立叶子模块代码目录仅允许 `include`、`sinclude`、`pinclude`、`src`；`test`、`cmake`、`third_party`、`samples`、`assets`、`template`、`generated`、`data` 为受控辅助目录；`modules/core`、`modules/function/*`、`modules/platform`、`modules/resource` 等聚合入口已列入明确例外清单，并新增配置期 `ModuleLayout.cmake` 门禁。
- [x] Asset Codec Catalog、Terrain/Tilemap/Physics3D Codec、Pak、生命周期、Mesh、Shell、Animation 和统一公共链接测试在 RelWithDebInfo/MSVC 下通过；Animation 测试仅因未设置 `LUX_ANIMATION_TEST_MODEL` 按既有约定跳过模型分支。
- [x] RelWithDebInfo 安装前缀完成 canonical `description`/`asset` consumer 配置、编译、链接、运行；Debug/RelWithDebInfo/Android 精确清理旧 Asset 头和产物；四个旧 component 查找均按预期失败。DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的 Windows x64/MSVC/RelWithDebInfo 全量构建及第二轮 `ninja: no work to do` 均通过；完整 CTest 命令执行但当前工程未注册 CTest 测试，owner 契约已直接运行通过；旧 target/header/export/visibility 全仓扫描归零。

## 本轮施工追踪（Asset 领域内聚、Pak 公共边界与 Engine Content）

> 以 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md` 为 SSOT。代码由 `1364810c`、`e7348155` 完成；验收证据见 `evidence/asset-domain-cohesion-f35e245a.md`。

- [x] `ASSETCOHESION-001` 保存 `f35e245a` 基线的公共头、target graph、安装 manifest 与资产/Pak fixture length+SHA。 **证据页已固化 32 个旧公共头、单一 Asset target/component 与 12 组 wire 指纹。**
- [x] `ASSETCOHESION-002` 建立 `engine/content` component，迁移冻结 Builtin UUID、M_Missing 与色板，删除 Resource owner。
- [x] `ASSETCOHESION-003` ECS Residency 改为可选 fallback material ID 注入，Runtime 在唯一装配点提供 Engine Content ID。
- [x] `ASSETCOHESION-004` Asset 公共头按 texture/material/mesh/model/animation/shader/script/storage 领域族迁移，无 shim。
- [x] `ASSETCOHESION-005` Asset 私有头、源码与 tests 镜像领域族，删除 `src/core`、`src/codecs`、`src/pak`。
- [x] `ASSETCOHESION-006` `TextureCodec/ModelCodec` 改名 `TextureSerDeser/ModelSerDeser`，旧类型与旧头归零。
- [x] `ASSETCOHESION-007` 拆出 `storage/AssetProvider.hpp`，并固定 Provider/VFS opaque bytes 与 AssetManager/AssetRef 引用账本边界。
- [x] `ASSETCOHESION-008` 公开 `storage/pak/PakArchive.hpp` writer/inspector API，保持 LUXPAK v2 wire。
- [x] `ASSETCOHESION-009` Toolchain 只保留 cook/publish 策略，删除 Asset `pinclude` 越界与重复 inspector 类型。
- [x] `ASSETCOHESION-010` 标准资产、Catalog、AssetRef、Provider/VFS、Pak、ECS fallback 与 Engine Content owner tests 通过。
- [x] `ASSETCOHESION-011` Asset/Engine Content public-link 与 installed consumers 通过，旧头/类型/私有跨界全仓归零。
- [x] `ASSETCOHESION-012` DEVELOPER/PLAYER/EDITOR/TOOLCHAIN RelWithDebInfo 全量构建、owner tests 和第二轮 no-op 验收完成。

## 本轮施工追踪（Asset Pipeline、Runtime Demand 与 Core Meta）

> 以 `ADR-20260821_Asset运行期需求与SerDeser边界.md`、`ADR-20260821_CoreMeta纯化与ECSRegistry归位.md` 为 SSOT。以下项目由 `ed5fb7eb` 实现；验收记录见 `evidence/asset-pipeline-core-meta-fe4422ba.md`。

- [x] `ASSETPIPE-001` Catalog 通过 manager-less SerDeser decode 完整、未注册 LuxAsset，删除 descriptor injector/decode 回调。
- [x] `ASSETPIPE-002` AssetManager 建立 shell-safe `installLoadedAsset()`，保持 AssetRef 账本、revision 与事件语义。
- [x] `ASSETPIPE-003` AssetLoadService 与同步 ensure 统一为 decodeAsset + installLoadedAsset，并保持 dedup/retry/ABA/close。
- [x] `ASSETPIPE-004` Script、Shader、Model、Scene 与全部标准 descriptor manager-less decode 完整内容。
- [x] `ASSETPIPE-005` LuxAsset 脱离 LuxObject，删除 untyped rawData/data API，Asset target 不再依赖 Core Meta。
- [x] `ASSETPIPE-006` Animation Resolver 归 Runtime packs，删除 ECS `AssetLoadFn` 与同步 test loader。
- [x] `ASSETPIPE-007` Runtime Script request system 使用 AssetClient，ECS ScriptSystem 不执行同步 IO。
- [x] `ASSETPIPE-008` Thumbnail provider 只报告缺失依赖，ThumbnailService 统一去重请求。
- [x] `ASSETPIPE-009` Registry、allocator 与 handles 原样迁入 ECS Core，旧 Meta API/头归零。
- [x] `ASSETPIPE-010` Core Meta/Serialization 安装闭包不含 EnTT，反射生成不依赖 LuxObject/EntityObject。
- [x] `ASSETPIPE-011` Core/Platform/Function/Resource 聚合目录使用显式列表，Description 删除未使用 Extension ABI 依赖。
- [x] `ASSETPIPE-012` owner tests、installed consumers、四 Profile 全量/no-op 构建与旧符号扫描全部通过。 **CTest 在四构建树均执行成功，但工程当前注册 0 项；契约测试均按 owner 可执行文件直接运行。**

## 统计

本清单当前共 **668** 个验收复选项：**321** 项完成，**320** 项有效待完成或部分完成，另有 **27** 项被现行 ADR 明确取代并保留未勾选作为历史记录。


---

# 当前代码到目标架构的迁移映射总表

> 以目录、CMake target 和类型为单位记录当前归属、目标归属、动作与责任文档

**执行文档 12 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | 原始文档 `09b2a82582550bcbe03afeef77d2591e1656a656`；Scene Asset `36ce56c6`；Asset 领域内聚 `1364810c`、契约 `e7348155`；lux-cxx `91b9233713bb713adeb16acaf681a84dd36e4546` |
| 基线日期 | 2026-08-19 |
| 文档更新 | 2026-08-21 |
| 适用对象 | 项目管理、架构负责人、实施者、迁移脚本维护者和代码评审者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决：** 资产与场景相关行以 `ADR-20260820_SceneAsset与Resource边界.md` 为准。旧 `AssetId/AssetTypeId/AssetStore` 目标已被取代；Resource 保留现有资产机制，Engine Scene 作为其上的领域 Asset，三类场景 Payload 二次归位 ECS owner。

> **2026-08-21 裁决：** Asset 领域目录、Storage/Pak 公共边界、Engine Content 与 ECS fallback 注入以 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md` 为准；`1364810c`、`e7348155` 已完成对应代码与契约施工。


## 使用规则

- 本表是迁移追踪清单，不替代各专题执行文档。
- 实施时为每行增加 Issue/Pull Request 链接和状态。
- 状态建议：`PENDING`、`IN_PROGRESS`、`COMPAT`、`DONE`、`BLOCKED`。
- 同一行涉及 wire format 时必须单独标注格式版本。

| 当前路径/符号 | 目标 | 动作 | Owner | 文档 |
| --- | --- | --- | --- | --- |
| modules/core/events | modules/core/events | KEEP/RENAME API | Core | 02 |
| lux::engine::core::events | lux::events | TARGET RENAME | Core | 09 |
| DomainEvents | Events | RENAME | Core | 02 |
| modules/core/log | modules/core/log | KEEP | Core | 02 |
| lux::engine::core::log | lux::log | TARGET RENAME | Core | 09 |
| modules/core/math | modules/core/math | KEEP/EXPAND | Core | 02 |
| modules/resource/spatial（已删除） | modules/core/math `Position.hpp` / `Grid.hpp` / `RelativePosition.hpp` | DONE — MOVE/DELETE；ECS-owned reflection sidecar | Core/ECS | 02/05 |
| modules/core/serialization | Core Archive/NameTable + `ecs/serialization` Component Archive | DONE — SPLIT；旧 Core reflected archive 已删除 | Core/ECS | 02/05/ADR-20260821-Serialization |
| modules/core/meta | lux-cxx reflection + ecs/core + engine/extensions/reflection | SPLIT/DELETE | Core/ECS/Engine | 02 |
| LuxObject | DELETE | DONE — 独立 `LuxAsset` 与 reflected-record identity 已取代 OO 根类 | ECS/Reflection | 02/03/ADR-20260821-Meta |
| EntityRegistry | `lux::ecs::Registry` | DONE — MOVE/RENAME，无 alias/forwarding header | ECS | 02/05/ADR-20260821-Meta |
| modules/core/extension_abi（已删除） | engine/extensions/api | DONE — MOVE/DELETE；Engine API 为唯一实体 owner，无兼容层 | Extensions | 02/06/ADR-20260821-ExtensionABI |
| ModuleAbi.hpp（已删除） | ExtensionAbi.hpp + descriptor/id/version/result 拆分头 | DONE — MOVE/DELETE；v4 类型名/layout/symbol 不变 | Extensions | 02/ADR-20260821-ExtensionABI |
| ContributionId（已删除） | DELETE；保留 domain-specific IDs | DONE — production/test/CMake 归零；不建立替代通用 ID | All domains | 02/06/ADR-20260821-ExtensionABI |
| modules/platform/common（已删除） | lux-cxx Core/Concurrent + Core Math + Description + Render Graph | DONE — SPLIT/DELETE；无 alias/shim/forwarding header | Core/Resource/Render | 02/03/04 |
| AtomicWait.hpp | `lux/cxx/concurrent/AtomicWait.hpp` | DONE — MOVE；Windows/Android 行为与编译契约 | lux-cxx Concurrent | 02 |
| FormatCompat.h | `lux/cxx/core/Format.hpp` | DONE — MOVE/DELETE generated header；std/libfmt/fallback 语义保留 | lux-cxx Core | 02 |
| Size2D.hpp / `Size2D` | `lux/engine/math/Extent.hpp` / `lux::math::Extent2u` | DONE — MOVE/RENAME；8-byte layout 固定 | Core Math | 02 |
| ImageEnums.hpp | `description/Image.hpp` + `render/graph/TextureAccess.hpp` | DONE — SPLIT；Dimension/Format 归 Description，Role 归 Render | Resource/Render | 02/03/04 |
| modules/platform/gapi | modules/platform/gapi | KEEP — ADR-20260821；公共 GAPI component 保留 | Platform | 02/04 |
| modules/platform/window | window core + window backends + render surface integration | SPLIT | Platform/Render | 02 |
| GlfwRuntime | GlfwLibrary/backend state | RENAME/INTERNAL | Platform | 02 |
| TrayIconWin32 | products/launcher or lux::tray | MOVE | Launcher/Platform | 02 |
| modules/platform/dynamic_library | modules/platform/dynamic_library | KEEP | Platform | 02 |
| modules/platform/filewatch | modules/platform/filewatch | KEEP | Platform | 02 |
| description/LayoutContract.hpp | render/shader/LayoutContract.hpp | MOVE | Render | 03/04 |
| description/RenderRepresentation.hpp | render/Representation.hpp | MOVE | Render | 03/04 |
| description/ImportedMaterialDesc.hpp | toolchain/model_import/ImportedMaterial.hpp | MOVE | Toolchain | 03 |
| description/MaterialGraphContract.hpp | authoring/material + toolchain/material + render/material | SPLIT | Authoring/Toolchain/Render | 03 |
| description/Script.hpp | function/script | MOVE/SPLIT | Script | 03/04 |
| modules/resource/deployment | render/config + engine/game/deployment | SPLIT/DELETE | Render/Game | 03 |
| RuntimeCapacity | RenderConfig/RenderCapacity + product options | SPLIT | Render/Game | 03 |
| RuntimeLaunchManifest | GameManifest | MOVE/RENAME | Game | 03/07 |
| modules/resource/entity_scene（已删除） | ecs/scene_format + engine/scene | DONE — LXES v1/Persistence v1 归 ECS；LXSC v1 归单一 Scene Asset component | ECS/Scene | 03/05/ADR |
| EntitySceneManifest / legacy DTO / Codec | SceneDescription/SceneAsset + EntitySection records | DONE — SceneDescription 二次改名并以 AssetFileHeader 包裹；内部 LXSC/LXES 字节不变 | ECS/Scene | 05/ADR |
| modules/resource/spatial3d_scene（已删除） | engine/spatial3d `SceneCatalog` | DONE — MOVE/DELETE；L3SC v1 字节不变 | Spatial3D | 03/06 |
| Authoring legacy World/Actor UUID wrapper | `WorldId` + `WorldActorId` | DONE — SPLIT；仅 Toolchain/Editor adapter 显式值转换 | Authoring/ECS/Scene | 05/06/08 |
| LXWA `SceneContribution` / `RequiredExtension` DTO | `WorldSceneFeatureRequest` + `WorldRequiredExtension` | DONE — Authoring owner，Toolchain leaf 转 cooked DTO | Authoring/Toolchain | 05/06 |
| modules/resource/classic_mesh（已删除） | `modules/function/render/standard_content` | DONE — MOVE/DELETE；LXCB v1 字节不变 | Render | 03/04 |
| modules/resource/terrain（已删除） | `ecs/terrain` | DONE — 历史 Resource 清零记录保留；值/Codec/tests 已二次归位，LXTT v1 不变 | ECS Terrain | 03/ADR |
| modules/resource/tilemap（已删除） | `ecs/tilemap` | DONE — 历史 Resource 清零记录保留；值/Codec/tests 已二次归位，LXTC v1 不变 | ECS Tilemap | 03/ADR |
| modules/resource/physics3d（已删除） | `ecs/physics3d` | DONE — 历史 Resource 清零记录保留；值/Codec/tests 已二次归位，LXPC v1 不变 | ECS Physics3D | 03/ADR |
| modules/resource/asset/{core,codecs,identity,pak}（已删除） | `modules/resource/asset/{src/core,src/codecs,src/pak}` + `include/pinclude/.../asset/{codecs,pak}` | DONE — FLATTEN/MERGE；统一 `lux::engine::resource::asset` target，旧四 target、目录和 visibility 宏删除；wire 格式不变 | Resource | 03/10 |
| asset_identity / asset_core / asset_codecs / asset_pak（已删除） | `lux::engine::resource::asset` / `lux_engine_asset` | DONE — TARGET MERGE；无 CMake alias、旧 export 或旧安装 component | Resource | 03/10 |
| asset_id_t | asset_id_t | KEEP — UUID wire/API；不创建 AssetId 同义类型 | Asset | ADR |
| EAssetType | EAssetType | DONE/KEEP — 显式 `uint32_t` 底层类型与稳定数值；Resource 已删除 Engine Scene/Section 成员名 | Asset/Scene | ADR |
| LuxAsset | LuxAsset | KEEP — 通用资产抽象 | Asset | ADR |
| TAsset<T> | TAsset<T> | KEEP — SceneAsset 复用 | Asset/Scene | ADR |
| AssetManager | AssetManager | KEEP — 通用驻留/引用/驱逐机制，不创建 AssetStore | Asset | ADR |
| AssetCodecCatalog | AssetCodecCatalog | DONE/EXPAND — 主/legacy magic、`findByMagic()`、type/magic/C++ identity 冲突校验与 legacy shell | Asset | ADR |
| AssetSerDeser / TAssetSerDeser | SceneAssetSerDeser 复用既有模板 | DONE/KEEP — Scene 只实现既有同步 SerDeser 接口 | Asset/Scene | ADR |
| Asset 根部领域头 + `codecs/` + `src/{core,codecs,pak}` | `asset/{texture,material,mesh,model,animation,shader,script,storage}` 镜像布局 | DONE — MOVE/RENAME，无 shim，wire 不变 | Asset | 03/ADR-20260821 |
| TextureCodec / ModelCodec | TextureSerDeser / ModelSerDeser | DONE — RENAME/DELETE OLD API | Asset | 03/ADR-20260821 |
| `AssetVfs.hpp` 内 Provider records | `storage/AssetProvider.hpp` + `storage/AssetVfs.hpp` | DONE — SPLIT；opaque bytes 不参与引用计数 | Asset | ADR-20260821 |
| `asset/pak/PakAssetProvider.hpp` + 私有 PakCodec | `asset/storage/pak/{PakArchive,PakAssetProvider}.hpp` + private wire | DONE — 公开 writer/inspector/provider，LUXPAK v2 不变 | Asset | ADR-20260821 |
| `modules/resource/asset/BuiltinAssetIds.hpp` | `engine/content/BuiltinAssetIds.hpp` | DONE — MOVE/DELETE OLD OWNER；UUID/色板不变 | Engine Content | 06/ADR-20260821 |
| ECS Residency 全局 `builtinMissingMaterialId()` | ResidencySubsystem constructor fallback ID | DONE — Engine Runtime 注入，ECS 不依赖 Engine Content | ECS/Runtime | 05/06/ADR-20260821 |
| Toolchain `PakCook` 直接访问 Asset pinclude/detail | 公共 `lux::asset::writePakFile/inspectPak` + Toolchain cook policy | DONE — REMOVE PRIVATE INCLUDE | Asset/Toolchain | ADR-20260821 |
| AssetVfs | asset::Vfs | RENAME | Asset | 03 |
| PakAssetProvider | asset::PakProvider | RENAME | Asset | 03 |
| render_client | lux::render | REFACTOR/TARGET RENAME | Render | 04 |
| render_graph | lux::render_graph | KEEP/RENAME | Render | 04 |
| render_vulkan | lux::render_vulkan | KEEP/EXPAND | Render | 04 |
| render_features | render_standard + internal object libs | SPLIT | Render | 04 |
| RenderBackendHost | render::Renderer | MOVE/RENAME | Render | 04/06 |
| RenderControlSession | Renderer internal | INTERNALIZE | Render | 04 |
| RenderFrameSession | Frame/internal | INTERNALIZE | Render | 04 |
| RenderUploadSession | UploadQueue/internal | INTERNALIZE | Render | 04 |
| ActionMapper/InputActionRegistry/InputContextStack | lux::input API with precise internals | REORGANIZE | Input | 04 |
| function::animation → asset | function::animation → Description + Core Math | DONE — REMOVE ASSET DEPENDENCY；installed consumer 不查找 Asset | Animation/Description | 04/10/ADR-20260821-Serialization |
| function::navigation → resource::spatial | function::navigation → math | DONE — DEPENDENCY FIX；安装闭包不再查找 Resource spatial | Navigation | 04 |
| ScriptHost | ScriptRuntime | RENAME | Script | 04 |
| modules/function/ui | ui core + imgui + backend integrations | SPLIT | UI | 04 |
| UISystem | UI | RENAME | UI | 04/08 |
| SceneViewportPanel | engine/editor/viewport/SceneViewport | MOVE | Editor | 04/08 |
| ecs/core dependency extension_abi（已删除） | ecs-owned ComponentSchemaId | DONE — REMOVE | ECS | 05 |
| ecs/core dependency asset | ecs/integration/assets + canonical `lux::engine::resource::asset` consumer closure | DONE — DEPENDENCY NORMALIZE；不再查找旧 Asset 子 target | ECS/Asset | 05/10 |
| ecs/core dependency entity_scene | ecs/scene_format | DONE — REPLACE；旧 target/component 已删除 | ECS | 05 |
| SceneServices | SceneServices | KEEP/INTERNALIZE USE | ECS | 05 |
| Schedule | Schedule | KEEP | ECS | 05 |
| EntityBatchDecoder | ecs::scene_format::SectionDecoder | MOVE/RENAME | ECS | 05 |
| EntityBatchMaterializer | Runtime `EntityBatchStager` staged materialization | SUPERSEDED — 不创建 RegistryArchive/SectionMaterializer 镜像 | Runtime/ECS | 05/ADR-20260821-Serialization |
| EntityBatchStager | Runtime Entity Section loading owner | KEEP — 不并入同步 Scene Codec component | Runtime/ECS | ADR |
| EntitySectionLoaderSystem | Runtime Entity Section loading owner | KEEP — Section 不注册为 Asset | Runtime/ECS | ADR |
| engine/runtime/execution | engine/execution | MOVE | Execution | 06 |
| AsyncRuntime | Executor | RENAME | Execution | 06 |
| AsyncRuntimeBuilder | ExecutorBuilder | RENAME | Execution | 06 |
| AsyncScope | TaskGroup | RENAME | Execution | 06 |
| AsyncFileService | FileIO | RENAME/REVIEW | Execution | 06 |
| engine/runtime/assets | engine/runtime/assets | KEEP — 公共 AssetManager 的异步编排适配器 | Runtime Assets | ADR |
| AssetLoadService | AssetLoadService | DONE/KEEP — 通过 Catalog magic 异步加载、去重、驻留与重载 SceneAsset | Runtime Assets | ADR |
| engine/runtime/extensions/loader | engine/extensions/loader | MOVE | Extensions | 06 |
| ExtensionModuleManager | ExtensionLoader | RENAME | Extensions | 06 |
| ModuleLifetime/ModuleLease | ExtensionLease | RENAME | Extensions | 06 |
| RuntimeContributionRegistrar | ExtensionRegistrar | MOVE/RENAME | Extensions | 06 |
| RuntimeRegistrationDraft | ExtensionDraft | RENAME | Extensions | 06 |
| EngineExtensions | ExtensionLoader + activation function + domain catalogs | SPLIT/DELETE | Extensions | 06 |
| SceneContributions | Scene Feature Catalog | MOVE/RENAME | Scene | 06 |
| RenderEffects | Render domain catalog | MOVE/RENAME | Render | 06 |
| engine/runtime/scene/core | engine/runtime/scene/core | KEEP — Scene Runtime 生命周期不进入 Scene Codec component | Runtime Scene | ADR |
| SceneRuntime | SceneRuntime | DONE/KEEP — 消费 typed SceneAsset 并持有 AssetRef | Runtime Scene | ADR |
| AssetCodecDescriptor decode / AssetDataInjector | `AssetCodecCatalog::decodeAsset` + manager-less SerDeser | DONE — DELETE/REPLACE，完整 owning LuxAsset 由 Manager 安装 | Asset | ADR-20260821-Pipeline |
| `AssetLoadFn` / `syncTestLoader` | Engine Runtime pack `AssetClient` demand | DONE — DELETE/MOVE，ECS 只消费 ready 数据 | Runtime | 05/06/ADR-20260821-Pipeline |
| `ThumbnailLoadFn` | `ThumbnailSpec` missing dependency IDs + ThumbnailService request | DONE — DELETE/REPLACE，Provider 保持纯规格生成 | Editor | 08/ADR-20260821-Pipeline |
| `lux::meta::EntityRegistry` / handles | `lux::ecs::Registry` / handles | DONE — MOVE/RENAME，allocator/publication/snapshot 合同保持 | ECS Core | 02/05/ADR-20260821-Meta |
| `LuxObject` / `EntityObject` | DELETE | DONE — DELETE，无 OO 反射根类 | Core Meta | 02/ADR-20260821-Meta |
| SceneContributionDescriptor | FeatureDescriptor | RENAME | Scene | 06 |
| SceneContributionHost | Features | RENAME/INTERNALIZE | Scene | 06 |
| SceneScriptRuntime | Script Feature + game::Session | SPLIT/DELETE | Scene/Game | 06 |
| engine/runtime/packs | scene/features or game/standard_features | SPLIT/DELETE | Scene/Game | 06 |
| engine/runtime/frame | product-private Frames | MOVE/DELETE TARGET | Game/Editor | 06 |
| FrameCoordinator | GameFrames/EditorFrames | SPLIT/INTERNAL | Game/Editor | 06 |
| MainCloseDriver | internal CloseTask driver | INTERNALIZE | Engine | 06 |
| engine/runtime | domain directories | DELETE | Engine | 06 |
| GameApplication | game::Game | MOVE/RENAME | Game | 07 |
| GameApplication::Impl | Game::State | RENAME/PRIVATE | Game | 07 |
| GameHost | DELETE; product main drives Game | DELETE | Player | 07 |
| RuntimeLaunchManifest game_pak/engine_pak | GameManifest gamePack/basePack | RENAME/ADAPTER | Game | 07 |
| LuxEditor | editor::Editor | RENAME | Editor | 07/08 |
| LuxEditor::Runtime | Editor::State + domain owners | SPLIT/DELETE | Editor | 08 |
| ProjectController | Workspace | REPLACE | Editor | 08 |
| SceneController | Documents/SceneDocument | REPLACE | Editor | 08 |
| ImportController | Content::import | REPLACE | Editor | 08 |
| AssetDeleteController | Content::remove | REPLACE | Editor | 08 |
| AssetRegistry | ContentIndex | RENAME/REFACTOR | Editor | 08 |
| AssetFileWatcher | ContentWatcher | RENAME | Editor | 08 |
| EditorShell | Workbench | RENAME/REFACTOR | Editor | 08 |
| EditorToolHost | Workbench::Panels | MERGE/DELETE | Editor | 08 |
| EditorTools | DELETE | DELETE | Editor | 08 |
| EditorPanelCatalog | PanelCatalog | RENAME | Editor | 08 |
| EditorAsyncService | domain operations | SPLIT/DELETE | Editor/Toolchain | 08 |
| EditorRenderInfra | precise dependencies | DELETE | Editor | 08 |
| FlowGraphPanel | FlowEditor | REFACTOR/RENAME | Editor | 08 |
| FlowForgeCompilerService | FlowCompiler | MOVE/RENAME | Toolchain | 08 |
| NodeRegistry::global | FlowNodeCatalog | DELETE/REPLACE | Flow | 08 |
| MaterialGraphPanel | MaterialEditor | REFACTOR/RENAME | Editor | 08 |
| MaterialPreviewHost | MaterialPreview + PreviewScene | REFACTOR/RENAME | Editor | 08 |
| ThumbnailService | Thumbnails + PreviewScene | REFACTOR/RENAME | Editor | 08 |
| EditorScene | SceneDocument + SceneViewport + game::Session | SPLIT | Editor | 08 |

## 2026-08-20 Scene Asset 边界修订状态

- `modules/resource/entity_scene` 与 `modules/resource/spatial3d_scene` 的 source path、target、component、include prefix、namespace 和安装产物均为 `DONE`，不存在 COMPAT alias/header。
- 当前 canonical owners 为 `lux::ecs::scene_format`、单一 `engine/scene` 中的 `SceneDescription/SceneAsset` 和 `lux::spatial3d::SceneCatalog`；`engine/runtime/entity_scene` 继续拥有 Runtime Section loading 职责。
- 文件格式未升级：LXSC v1、LXES v1、L3SC v1、Persistence Journal v1、LXWA v4 及子文档版本由 Golden/Wire contract 冻结。
- 迁移期四项 Python boundary scanner 及其 CI Job 已由项目决定退役；当前边界依据是 owner 编译/链接/Golden/Wire tests 和 CMake target DAG。
- `ContributionId` 行已为 `DONE`：该通用 ID 不属于 Extension ABI v4 descriptor 布局；删除不改变任何 v4 对象布局或导出 symbol。

## 2026-08-20 Resource Content 清零状态

- `modules/resource/classic_mesh`、`terrain`、`tilemap`、`physics3d` 的 source path、target、component、include prefix 与安装产物均为 `DONE`，不存在 COMPAT alias/header。
- Terrain、Tilemap、Physics3D 曾完成 Resource 内容组件清零；现已按 ADR 二次归位对应 ECS 领域。Classic Mesh 的值与 LXCB v1 Codec 继续由 Function `render_standard_content` 所有。
- 四种确定性 fixture 的长度/SHA-256 与 decode/re-encode 已冻结；未增加 wire version，magic、字段顺序、UUID、浮点和整数编码保持不变。
- `ASSETSDK-019` 已完成：公共头、私有 Description Codec、实现与测试按资产领域族共同组织；未引入 AssetStore 或全局 package namespace 重构。

## 2026-08-21 Asset 领域内聚完成状态

- Asset 以 texture/material/mesh/model/animation/shader/script/storage 为唯一领域布局；旧根部领域头、`codecs/`、`pak/` 和 `src/{core,codecs,pak}` 归零。
- Provider/VFS/Pak 统一位于 storage；公共 Pak writer/inspector/provider 不解释 Engine 语义，Toolchain 只保留 cook/publish policy。
- Builtin UUID、M_Missing 和色板由轻量 `engine/content` 所有；ECS fallback 通过 Runtime 构造注入，ECS link closure 不含 Engine Content。
- 11 类标准资产和 1000-entry LUXPAK v2 fixture 已冻结长度/SHA；AssetFileHeader v1 兼容与 corrupt/truncated 契约通过。

## 2026-08-20 Spatial 基础值归 Math 与 Resource 最终清零状态

- `modules/resource/spatial` 的 source path、target、component、include prefix、namespace 与安装导出均为 `DONE`；不保留 COMPAT alias/header。
- `lux::math::Position2d` / `Position3d` / GridCoord 与 `relativeFloat()` 由纯 Core Math 所有；ECS Runtime Reflection 由 consumer-owned external reflection adapter 与 traits sidecar 所有。
- Navigation、ECS、Authoring、Runtime、Spatial3D、Toolchain 与 Editor 消费者已直接依赖 Core Math；`modules/resource` 一级目录与 CMake 注册只剩 `description` 和 `asset`。
- 除 FlowGraph v3 的反射类名 payload 使用新 canonical Math 名称并拒绝历史 Resource 名称外，LXWA/LXAI/LXAD/LXIP/LXTP/LXTL/LXPP、Descriptor Index、LXES/Persistence、L3SC 的版本与字节契约均保持不变。

## 状态扩展模板

实施仓库可把本表复制为机器可读 YAML：

```yaml
- current: modules/core/extension_abi (deleted)
  target: engine/extensions/api
  action: MOVE_DELETE
  owner: extensions
  document: "02,06"
  status: DONE
  compatibility:
    target_alias: false
    forwarding_headers: false
    remove_milestone: EXTABI
  format_or_abi:
    kind: extension_abi
    version: 4
```

持续集成可验证：

```text
status=DONE → current path/symbol 不得存在
status=COMPAT → 只允许 forwarding/alias，不允许实现
status=PENDING → legacy report 允许但不能增长
```

## 最终验收

- [ ] 表中所有行都有 owner。
- [ ] 表中所有行都有 Pull Request 或批准的 BLOCKED 说明。
- [ ] 所有 `DELETE` 当前路径已从安装与构建树消失。
- [ ] 所有 `COMPAT` alias/header 已按里程碑删除。
- [ ] 所有 ABI/格式迁移附 Golden/fixture 测试。
- [ ] 无未登记的新旧双重概念。


---

# 当前代码事实索引

> 为实施方提供提交基线上的关键文件锚点、当前职责和对应施工文档

**执行文档 13 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | 原始索引 `09b2a82582550bcbe03afeef77d2591e1656a656`；Pipeline/Core Meta `ed5fb7eb`；Component Archive `d1ead288`；Extension ABI 实施 `c56efbc4` |
| 基线日期 | 2026-08-19 |
| 文档更新 | 2026-08-21 |
| 适用对象 | 实施者、代码评审者、迁移脚本维护者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> 资产与 Scene 的现行事实由 `ADR-20260820_SceneAsset与Resource边界.md` 修订：现有 AssetManager/SerDeser/Catalog/VFS/Pak 保留；Scene 已收敛为单一 Engine Scene Asset component；三类 Resource 场景 Payload 已二次归位 ECS owner。

> **2026-08-21 当前事实：** Asset 已按领域族与 storage 组织；`BuiltinAssetIds.hpp` 已迁至 `engine/content`；Toolchain 只调用公共 Pak API；Provider/VFS 可读取 opaque records，但只有 AssetManager/AssetRef 参与驻留引用账本。

> **2026-08-21 当前事实：** Catalog 通过 manager-less SerDeser 产出完整 owning `LuxAsset`，LoadService 与同步 ensure 共用同一 decode/install 路径；Runtime packs 显式提出 Animation/Script 需求，Thumbnail 只报告依赖。Registry、allocator 与 handles 已归 ECS Core，Core Meta/Serialization 与 Asset 的安装闭包分别不再包含 EnTT 与 Core Meta。

> **2026-08-21 当前事实：** Core Serialization 只含 Archive/NameTable，安装闭包不含 Meta/Eigen。Reflected tagged archive 已归 ECS `component_archive`；tag 48 为 UUID，只有显式 `asset_type=` annotation 表达资产引用。不创建 RegistryArchive。

> **2026-08-21 Extension ABI 当前事实：** `engine/extensions/api` 是 v4 实体与独立安装 component 的唯一 owner；`modules/core/extension_abi` 与通用 `ContributionId` 已删除。ABI v4 namespace/layout/ordinal/fingerprint、ABI-facing registrar 名称和三个 symbol string 保持不变。Authoring 保持 source DTO，并在 Toolchain/Editor 边界显式转换。


## 使用说明

本索引保留提交 `09b2a825...` 的原始文件锚点，并按 Scene Asset `36ce56c6`、Asset 领域内聚 `1364810c` 与契约 `e7348155` 的实际施工结果更新当前事实。实施分支若继续前移：

1. 先按旧路径查找；
2. 若路径变化，按类型名和 CMake target 查找；
3. 更新本索引；
4. 不以旧行号作为施工依据。

| 当前锚点 | 当前事实 | 施工文档 |
| --- | --- | --- |
| `modules/core/CMakeLists.txt` | 只安装 events/log/math/meta/serialization；无 Extension API | 01/02 |
| `modules/core/extension_abi`（已删除） | 旧 ABI owner、target、component、include 与 export 全部归零 | 02/06/ADR-20260821-ExtensionABI |
| `ContributionId`（已删除） | 通用类型与 helper 在 production/test/CMake 归零；领域 ID 为唯一选择 | 02/06/ADR-20260821-ExtensionABI |
| `engine/extensions/api` | ABI v4 实体与 `lux-engine-extensions/extension_api` 唯一 owner；直接导出 lux-cxx core/abi | 02/06/ADR-20260821-ExtensionABI |
| `modules/core/meta/CMakeLists.txt` | Runtime reflection 与 codegen scripts；不查找、包含或链接 EnTT | 02/05/ADR-20260821-Meta |
| `ecs/core/include/lux/engine/ecs/{Entity,Registry,RegistryMemoryResource}.hpp` | ECS-owned Entity/Registry/handles/allocator/publication/snapshot API；旧 Meta owner 已删除 | 02/05/ADR-20260821-Meta |
| `modules/core/serialization/CMakeLists.txt` | 只拥有 Archive/NameTable/readSpan；PUBLIC binary/stduuid，无 Meta/Eigen | 02/ADR-20260821-Serialization |
| `ecs/serialization` | `lux::engine::ecs::component_archive`；拥有 reflected tagged wire、bounded compatible/exact reader、详细 expected 与资产 annotation 判定 | 05/10/ADR-20260821-Serialization |
| `modules/platform/common`（已删除） | AtomicWait/Format/Extent/Image/Texture Role 已归精确 owner；旧 target/component/include/namespace 与安装产物归零 | 02/03/04 |
| `lux-cxx/core/.../Format.hpp`、`lux-cxx/concurrent/.../AtomicWait.hpp` | 跨平台 Format compatibility 与 64-bit atomic deadline wait 的唯一 owner；Windows/Android 编译契约通过 | 02 |
| `modules/core/math/.../Extent.hpp` | `lux::math::Extent2u` 唯一 owner；standard-layout/trivially-copyable/8-byte layout 契约 | 02 |
| `modules/resource/description/.../Image.hpp` | `lux::rdesc::ETextureDimension/ETextureFormat` owner；ordinal 与 32-bit wire/layout 语义不变 | 02/03 |
| `modules/function/render/graph/.../TextureAccess.hpp` | `lux::render::ETextureRole` owner；Render Graph 可 headless 独立安装和链接 | 02/04 |
| `modules/platform/gapi/CMakeLists.txt` | 保留的公共 Vulkan interface target 与 wrapper headers；Render 可消费但不取得所有权 | 02/04 / GAPI ADR |
| `modules/platform/window/CMakeLists.txt` | GLFW/Android、Vulkan Header、Tray Icon | 02/04 |
| `modules/resource/CMakeLists.txt` | 只显式添加并安装 description 与 asset 家族；无自动目录枚举或其他一级 component | 01/03 |
| `modules/resource/description/CMakeLists.txt` | 只拥有通用 Description 值；不再拥有 Terrain、Tilemap、StaticCollider3D 场景 Payload | 03/ADR |
| `modules/resource/description/.../LayoutContract.hpp` | Render descriptor layout contract | 03/04 |
| `modules/resource/description/.../ImportedMaterialDesc.hpp` | Model Importer → Material Graph 中间结构 | 03 |
| `modules/resource/asset/CMakeLists.txt` | 唯一 `lux::engine::resource::asset` 组件；拥有通用 AssetManager/SerDeser/Catalog/VFS/Pak；不含 Engine Scene 或 ECS 场景 Payload Codec | 03/10/ADR |
| `modules/resource/asset/include/lux/engine/resource/asset/` | 核心 Asset API + texture/material/mesh/model/animation/shader/script/storage 领域公共头；无 Engine 默认内容 | 03/10/ADR-20260821 |
| `modules/resource/asset/pinclude/lux/engine/resource/asset/` | AssetManager detail、各领域 Description Codec 与 storage/pak 私有 wire 头；Engine 不可见 | 03/10/ADR-20260821 |
| `modules/resource/asset/src/{animation,material,mesh,model,script,shader,storage,texture}` | 唯一 Asset 库的镜像领域实现；旧 `src/{core,codecs,pak}` 不存在 | 03/10/ADR-20260821 |
| `engine/content/include/lux/engine/content/BuiltinAssetIds.hpp` | Engine 冻结 UUID、M_Missing 身份与演示色板的唯一 owner；轻量 `lux::engine::content` component | 06/ADR-20260821 |
| `engine/toolchain/asset/cook/src/PakCook.cpp` | 使用公共 `lux::asset::writePakFile/inspectPak`，只保留目录扫描、Catalog 判断、冲突诊断与 cook/publish policy | 03/06/ADR-20260821 |
| `ecs/render/.../ResidencySubsystem.hpp` | 接收可选 fallback material ID；Runtime 在 `RenderSceneIntegration` 注入 Engine Content ID，nil 支持 headless/通用 consumer | 05/06/ADR-20260821 |
| `modules/resource/asset/test/asset_wire_contract_test.cpp` | 11 类标准资产 length/SHA、decode/re-encode、AssetFileHeader v1 与损坏输入契约 | 03/10/ADR-20260821 |
| `modules/resource/asset/test/storage/` | Provider/VFS opaque record、引用账本隔离、LUXPAK v2 writer/inspector/provider/paged/corrupt 契约 | 03/10/ADR-20260821 |
| `modules/resource/deployment`（已删除） | canonical owner 为 `engine/game/deployment` | 03/07 |
| `modules/resource/entity_scene`（已删除） | LXES/Persistence owner 为 `ecs/scene_format`；LXSC/Scene Asset owner 为单一 `engine/scene`；无 shim/alias | 03/05/ADR |
| `modules/resource/spatial`（已删除） | Position/Grid/relativeFloat owner 为纯 Core Math；无 shim/alias | 02/05 |
| `modules/core/math/include/lux/engine/math/{Position,Grid,RelativePosition}.hpp` | `lux::math` 大坐标空间值与相对浮点转换；不依赖 Meta/ECS/Resource | 02 |
| `ecs/core/include/lux/engine/ecs/reflection/SpatialValueReflection*.hpp` | Math 空间值的 ECS-owned external reflection adapter/traits；生成 ecs_meta/ecs_lua_meta sidecar | 05 |
| `modules/resource/spatial3d_scene`（已删除） | L3SC/Scene Catalog owner 为 `engine/spatial3d`；无 shim/alias | 03/06 |
| `modules/resource/classic_mesh`（已删除） | Classic Mesh 值与 LXCB v1 Codec owner 为 Function `render_standard_content`；无 shim/alias | 03/04 |
| `modules/resource/terrain`（已删除） | Terrain 值/Codec/tests owner 为 `ecs/terrain`；LXTT v1 不变 | 03/ADR |
| `modules/resource/tilemap`（已删除） | Tilemap 值/Codec/tests owner 为 `ecs/tilemap`；LXTC v1 不变 | 03/ADR |
| `modules/resource/physics3d`（已删除） | StaticCollider3D 值/Codec/tests owner 为 `ecs/physics3d`；LXPC v1 不变 | 03/ADR |
| `engine/scene/CMakeLists.txt` | 单一 `lux::engine::scene::scene` component；公开 SceneDescription/SceneAsset/SceneAssetSerDeser | 05/06/ADR |
| `engine/scene/src/SceneDescriptionCodec.cpp` + `SceneAssetSerDeser.cpp` | LXSC v1 data Codec 字节不变；标准 Scene Asset 以 AssetFileHeader 包裹，裸 LXSC 只读兼容 | 05/06/ADR |
| `engine/spatial3d/.../SceneCatalog.hpp` | Engine-owned `SceneCatalog`、L3SC v1 Codec 与 canonical Scene/ECS IDs | 03/06 |
| `engine/authoring/world/.../WorldIdentifiers.hpp` | Authoring-owned `WorldId`、`WorldActorId`、Feature/Extension stable-name IDs，不与 ECS/Scene Asset ID 隐式转换 | 05/06/08 |
| `engine/authoring/world/.../WorldSource.hpp` | `WorldSceneFeatureRequest` / `WorldRequiredExtension` 为 Authoring DTO；Player/Runtime 不读取 LXWA | 05/06/08 |
| `modules/function/render/client/CMakeLists.txt` | Render protocol 按用途直接依赖 Core Math/Description/Render Graph/lux-cxx；无 Platform Common | 04 |
| `modules/function/render/standard_content/CMakeLists.txt` | 轻量 Render standard content owner：Classic Mesh 值与 LXCB v1 Codec；不依赖 Vulkan/GLFW/ECS/Engine | 03/04/10 |
| `modules/function/render/vulkan/CMakeLists.txt` | Vulkan server/renderer/resources/scene/targets | 04 |
| `modules/function/render/features/CMakeLists.txt` | 大量标准/工具/地形/点云 Feature 与 codegen | 04 |
| `modules/function/animation/CMakeLists.txt` | PUBLIC Description + Core Math；不再查找或链接 Resource Asset | 04/10 |
| `modules/function/input/CMakeLists.txt` | PUBLIC platform window/GLFW closure | 04 |
| `modules/function/script/core/.../ScriptHost.hpp` | 脚本 backend/module/invoke dispatcher | 04 |
| `modules/function/ui/CMakeLists.txt` | 基础 UI PUBLIC ImGui GLFW/Vulkan，包含 SceneViewportPanel | 04/08 |
| `ecs/CMakeLists.txt` | ECS 层定义与领域目标列表 | 05 |
| `ecs/core/CMakeLists.txt` | 拥有 Registry 与 EnTT component reflection adapter；已退出 Resource Asset/entity_scene/spatial，installed consumer 不导入 Resource | 05/ADR-20260821-Meta |
| `engine/runtime/CMakeLists.txt` | execution/assets/extensions/entity_scene/scene/render/packs/frame 聚合 | 06 |
| `engine/runtime/execution` | AsyncRuntime/Builder/Scope/MainThreadMailbox | 06 |
| `engine/runtime/assets` | AssetLoadService 执行 VFS open → Catalog decode → Manager install，保持 dedup/retry/backoff/ABA/close；不建立第二套资产系统 | 06/ADR-20260821-Pipeline |
| `modules/resource/asset/.../AssetCodecCatalog.hpp` | `decodeAsset()` 通过 descriptor factory 与 manager-less SerDeser 返回完整 owning `LuxAsset`；无 injector/decode 回调 | 03/06/ADR-20260821-Pipeline |
| `engine/runtime/packs/scene2d`、`scene3d` | 私有 Animation Resolver 使用 AssetClient 请求并在每帧重查 typed Asset；ECS Animation 只保留纯采样 | 05/06/ADR-20260821-Pipeline |
| `ecs/script/.../ScriptSystem.cpp` + `engine/runtime/scene/script` | ECS ScriptSystem 只消费 ready ScriptAsset；Runtime request system 负责异步需求并先于 ScriptSystem 运行 | 05/06/ADR-20260821-Pipeline |
| `engine/editor/.../thumbnail` | Provider 只构造含缺失依赖 ID 的 ThumbnailSpec；ThumbnailService 去重并通过 AssetClient 请求 | 08/ADR-20260821-Pipeline |
| `engine/runtime/extensions/contribution_host/.../RuntimeContributionRegistrar.hpp` | components/scene/render/async 四 registrar + draft | 06 |
| `engine/runtime/extensions/loader/.../ExtensionModuleManager.hpp` | 动态库 Extension loader | 06 |
| `engine/runtime/scene/core/.../SceneRuntime.hpp` | World+Schedule+Services owner 与 close；消费 typed SceneAsset 并持有 AssetRef | 06/ADR |
| `engine/runtime/entity_scene` | decoder/materializer/stager/loader system/service/store | 05/06 |
| `engine/runtime/entity_scene/test/entity_section_wire_compatibility_test.cpp` | canonical LXES v1 / Persistence Journal v1 frozen fixtures，不构造 legacy Resource DTO | 05/10 |
| `engine/scene/test/scene_asset_contract_test.cpp` | canonical LXSC v1 frozen fixture、裸格式升级、包裹 data 区逐字节一致、validation 与 public-link 契约 | 05/10/ADR |
| `engine/spatial3d/test/scene_catalog_contract_test.cpp` | canonical L3SC v1 frozen fixture、异常输入与 limits 契约 | 06/10 |
| `engine/authoring/world/test/world_source_codec_test.cpp` | LXWA v4、LXAI/LXAD/LXIP v2、LXTP/LXTL/LXPP v1 固定长度+SHA-256 与 decode/re-encode 契约 | 05/10 |
| `.github/workflows/architecture-recovery.yml` | 保留 `source-snapshot` 与 CMake 架构 DAG；四项 Python boundary Job 已按项目决定删除，改用 owner contract tests | 09/10 |
| `engine/hosts/game_application/.../GameApplication.hpp` | 平台中立产品组合根 | 07 |
| `engine/hosts/game_application/src/GameApplication.cpp` | 约 71KB 顶层装配与帧/关闭逻辑 | 07 |
| `engine/hosts/player/.../GameHost.hpp` | GLFW 平台适配 Host | 07 |
| `engine/editor/CMakeLists.txt` | Controllers、Shell、AsyncService、Panels、Preview 大聚合目标 | 08 |
| `engine/editor/src/app/LuxEditor.cpp` | Editor 组合根与 Runtime storage | 08 |
| `engine/editor/src/app/EditorShell.cpp` | Panel wiring + Asset authoring 业务 | 08 |
| `engine/editor/src/app/EditorAsyncService.cpp` | Editor 全领域异步 Operation 聚合 | 08 |
| `engine/editor/src/panels/FlowGraphPanel.hpp` | Graph+Asset save+compile hook+global NodeRegistry | 08 |
| `engine/editor/src/panels/MaterialGraphPanel.hpp` | Graph+compile+preview+asset+instance 状态 | 08 |
| `engine/editor/src/thumbnail/MaterialPreviewHost.*` | 私有 preview Scene owner | 08 |
| `engine/editor/src/thumbnail/ThumbnailService.*` | 缩略图私有 runtime/preview 流程 | 08 |

## 建议的搜索命令

```bash
git grep -n "RuntimeContributionRegistrar"
git grep -n "LuxEditor::Runtime"
git grep -n "setAssetServices"
git grep -n "setPrecompileHook"
git grep -n "NodeRegistry::global"
git grep -n "lux::engine::resource::deployment"
git grep -n "lux::engine::core::extension_abi"
git grep -n "lux::engine::platform::common"
git grep -n "lux::engine::platform::gapi"
git grep -n "lux/engine/resource/entity_scene"
git grep -n "lux/engine/resource/spatial"
git grep -n "lux/engine/resource/spatial3d_scene"
git grep -n -E "lux/engine/resource/(classic_mesh|terrain|tilemap|physics3d)"
git grep -n -E "lux::(entity_scene|spatial3d_scene)"
```

CMake：

```bash
git grep -n "runtime_render_backend_host" -- '*CMakeLists.txt'
git grep -n -E "asset_(identity|core|codecs|pak)|resource/asset/(core|codecs|identity|pak)" -- '*CMakeLists.txt' '*.cpp' '*.hpp' '*.h'
git grep -n "extension_abi" -- '*CMakeLists.txt'
git grep -n "resource::spatial" -- '*CMakeLists.txt'
git grep -n -E "resource::(entity_scene|spatial3d_scene)" -- '*CMakeLists.txt'
git grep -n -E "resource::(classic_mesh_content|terrain_content|tilemap_content|physics3d_content)" -- '*CMakeLists.txt'
```

## 事实索引维护闸门

- [ ] 每个大规模 MOVE 后更新路径。
- [ ] 删除旧文件时将对应行标记 DONE，而不是静默删除历史。
- [ ] 新发现的错位模块先加入本索引，再进入 12 映射表。
- [ ] 当前事实和目标建议分栏，不把目标误写成已实现状态。


---

# v2 相对 v1 的架构修订说明

## 2026-08-21：Extension ABI v4 Owner 与 Core 清零

- `2a916295` 先裁决 owner 与冻结表面；`c56efbc4` 已使 `engine/extensions/api` 成为唯一实体 owner，并删除旧 Core component。
- ABI v4 的 descriptor、ordinal、fingerprint、registrar 对象布局与导出 symbol 冻结；本轮只改变 owner/include/target。
- `RuntimeContributionRegistrar` 等 ABI-facing 类型在 v4 内不改名；相关旧 Checklist 重命名项由本裁决取代。
- 通用 `ContributionId` 不在 v4 descriptor 布局中且已无 production 消费，可以直接删除。
- 静态 reflection pending chain 的移除需要独立 codegen/plugin-entry 裁决，不夹带进 owner 搬迁。
- Authoring Project Manifest 保持 source DTO；Toolchain/Editor 在唯一上层边界显式转换为 Engine Extension API，避免 AUTHORING→RUNTIME 反向依赖。
- 四 Profile 全量/no-op、动态 DLL transaction、实际 Physics2D exports、installed consumer 与旧 component 反向查找均已通过。

## 2026-08-21：Core Serialization 与 ECS Component Archive

- Core Serialization 收敛为 Archive/NameTable/Byte primitives，移除 Reflection/Eigen 公共闭包。
- reflected tagged-property archive 整体归 ECS `component_archive`；不创建 RegistryArchive。
- tag 48 保持 wire ordinal，源码语义从 AssetRef 改为 UUID；资产引用只由显式 `asset_type=` annotation 表达。
- Unknown Component schema 在 Authoring/Toolchain/Runtime 拒绝；已知 Component 的 compatible reader 仍可跳过未知字段。
- `d9d3619b` 先落地文档裁决，`d1ead288` 完成代码迁移；四 Profile、owner executables、三类 installed consumer 与安装前缀同步通过。

## 2026-08-21：Asset 领域内聚、Pak 公共边界与 Engine Content

- 接受 `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`。
- Asset 从 `core/codecs/pak` 横向分区改为领域族组织，Storage 作为语义领域容纳 Provider/VFS/Pak。
- Pak v2 reader/writer/inspector 是公共 Modules Asset SDK；Toolchain 只拥有 cook/publish 策略。
- `BuiltinAssetIds.hpp`、M_Missing 与内置色板归 `engine/content`；ECS fallback 改为 Engine Runtime 注入。
- 保持 AssetFileHeader v1/v2、所有资产 wire、冻结 UUID/颜色和 LUXPAK v2 字节不变。

## 2026-08-21：Asset 领域内聚实施完成

- `1364810c` 完成 Asset 领域布局、Storage/Pak 公共 API、Engine Content owner 与 fallback 注入；`e7348155` 完成 wire/storage 契约。
- 11 类标准资产与 1000-entry LUXPAK v2 fixture 固定 length/SHA；decode/re-encode、v1 读取、corrupt/truncated 与引用账本隔离测试通过。
- Toolchain 不再包含 Asset `pinclude`；Resource/ECS 不再拥有或反向依赖 Engine Builtin 身份。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的 Windows RelWithDebInfo 全量构建与第二轮 no-op 通过；installed Asset/Content consumers 通过。

## 2026-08-20：Scene Asset 与 Resource 边界修订

- 接受 `ADR-20260820_SceneAsset与Resource边界.md`。
- 保留公共 `asset_id_t`、`LuxAsset/TAsset`、`AssetManager`、`TAssetSerDeser`、Catalog、Provider、VFS 与 Pak。
- 取消新建 `AssetId`、`AssetTypeId`、`engine/assets/AssetStore` 和第二套资产系统的目标。
- 将 Scene 定义为既有资产机制上的 Engine-owned Asset；`engine/scene` 只负责同步数据、验证和 SerDeser。
- 将 Terrain/Tilemap/Physics3D 场景 Payload 的最终 owner 改为对应 ECS 领域，保留历史 Resource 内容组件清零记录。
- 用 `SCENEASSET-*` 条目追踪二次归位，不把被取代的旧目标伪装为已完成。

## 2026-08-20 Scene Asset 实施完成

- `SCENEASSET-001..020` 已完成并通过 owner contract、四 Profile 全量构建、安装 consumer 与旧符号归零验收。
- `engine/scene/api` 与 `engine/scene/package` 已删除，canonical owner 为单一 `engine/scene` component。
- Terrain、Tilemap、StaticColliderBatch3D 值/Codec/tests 已二次归位对应 ECS 领域，五种既有内部 wire version 均未升级。
- CTest 当前未注册测试；验收结果明确来自独立 owner test executables，不将 0 项 CTest 误报为覆盖。

> 明确废止的旧建议、保留的上层裁决以及新版施工顺序变化

**执行文档 R · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-20 |
| 适用对象 | 此前文档使用者、项目负责人、代码评审者 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。


## 废止

- 废止 `engine/module/include/lux/module/Modules.hpp`。
- 废止把 Extension 全面改名为 Module。
- 废止把 Scene/Render/ECS/Operation 统一注册到一个公共 Module 容器。
- 废止让公共 Resource 承载 Deployment、EntityScene、WorldPartition。
- 废止把 `AssetManager` 原样改名后继续作为公共 Resource owner。
- 废止让 ECS Core 依赖 Engine Extension ABI 与 Engine Asset 管理。
- 废止先重构 Game/Editor、后处理公共模块边界的施工顺序。

## 保留

- 不创建公开 EngineRuntime。
- Game 与 Editor 是独立产品。
- Editor Play 复用 game::Session。
- Schedule 唯一拥有 System。
- Panel 构造注入精确依赖。
- 命令与事实分离。
- 显式关闭协议。
- Workspace/Workbench/Documents 拆分。
- Flow/Material/Script 从 Panel 中拆出 Compiler/Document/Commands。

## 新增

- `modules/` 作为独立 SDK 分发边界的准入规则。
- Resource 只保留 Description/Asset 的硬闸门。
- 格式归语义所有者的三问法。
- Build-time Generator 依赖不污染 Runtime 的规则。
- Extension ABI 单独 Engine SDK。
- 488 项施工 Checklist。
- 当前代码事实索引与 123 条迁移映射。

## 2026-08-21 Asset Pipeline 与 Core Meta 裁决

- `AssetSerDeser` 成为唯一具体 Codec 多态接口；Catalog 解码完整对象，LoadService 编排，Manager 安装并维护账本。
- 删除 `AssetDataInjector`、`AssetLoadFn`、`ThumbnailLoadFn` 等退化回调；AssetRef 明确不触发 IO。
- Animation/Script/Thumbnail 的运行期需求归 Engine Runtime/Editor service 装配，ECS 不执行同步 IO。
- Registry、allocator 与 handles 归 ECS Core；Core Meta 删除 EnTT、LuxObject、EntityObject 与虚构反射根类。
- `MODULES_SDK` Profile 提案废止，改用现有四个 Profile 的 installed consumers 验证公共包闭包。

实施提交 `ed5fb7eb` 已完成上述裁决；owner tests、四 Profile 全量/no-op 构建、三类 installed consumer 与旧符号/依赖闭包扫描均通过。完整证据见 `evidence/asset-pipeline-core-meta-fe4422ba.md`。

## 2026-08-21 GAPI 保留与单体 Input 裁决

- 保留 `modules/platform/gapi` 的公共 target、component、namespace 与低层 Vulkan wrapper 所有权，废止并入 Render 后删除旧 owner 的目标。
- Input 只保留 `lux::engine::function::input` 一个 target；平台差异由 CMake 选择私有 GLFW/Android 源实现。
- Window 只产出原始事件，`lux::input::Input` 统一拥有 Snapshot、ActionMapper、唯一 ActionRegistry 与 ContextStack。
- 禁止新增 Input Adapter target/interface、backend factory、catalog 或运行期多态。


---

# ADR：Scene Asset 与 Resource 边界

**状态：** Accepted

**日期：** 2026-08-20

**适用基线：** `codex/scene-asset-redesign` 及后续提交
**取代范围：** 执行文档 v2 中关于新建 `AssetId`、`AssetTypeId`、`engine/assets/AssetStore`，以及把场景领域 Payload 永久放在 Resource 的目标

## 1. 背景

`modules/` 是可以被外部项目独立使用的通用库，`engine/` 才承载 LUX Engine 的场景、ECS 装配和产品语义。现有 Resource Asset 已经提供完整且抽象的资产机制：

- `asset_id_t = uuids::uuid`；
- `LuxAsset`、`TAsset<T>` 与 `TAssetSerDeser<Config>`；
- `AssetManager`、`AssetRef`、revision、驻留票据、驱逐与广播；
- `AssetCodecCatalog`、Provider、VFS、Loose/Pak；
- `engine/runtime/assets` 对现有异步执行管线的加载编排。

因此，把 `AssetManager` 复制或上移为第二套 `AssetStore`，以及仅为改名引入 `AssetId`、`AssetTypeId`，都会制造重复概念而不能改善依赖方向。

另一方面，Scene、Entity Section、Terrain、Tilemap 和 Physics3D 场景 Payload 带有明确的 Engine/ECS 语义。它们不应由通用 Resource 解释，但 Scene 本身仍应复用公共资产机制获得 VFS、Pak、异步加载和驻留能力。

## 2. 决议

### 2.1 公共 Resource Asset 保持完整

`modules/resource/asset` 继续拥有通用的：

```text
asset_id_t
LuxAsset / TAsset<T>
AssetManager / AssetRef
TAssetSerDeser
AssetCodecCatalog
Provider / VFS / Loose / Pak
```

不创建：

```text
AssetId
AssetTypeId
engine/assets
AssetStore
第二套资产 Registry 或全局可变 Codec Registry
```

`EAssetType` 保留为现有资产头的稳定数值类型。Resource 只定义通用资产的枚举成员；Engine-owned 类型可以保留兼容数值，但不能把 Engine 名称和语义写回 Resource。

### 2.2 Engine Scene 是公共资产机制上的领域资产

`engine/scene` 是一个组件，只拥有：

```text
SceneDescription
SceneAsset : lux::asset::TAsset<SceneDescription>
SceneAssetSerDeser : lux::asset::TAssetSerDeser<std::monostate>
Scene Feature 领域 ID
LXSC v1 的同步、bounded、无副作用验证与 Codec
```

它不拥有：

```text
IO 线程或异步执行器
第二套 AssetManager
entt::registry 生命周期
SceneRuntime 生命周期
Editor 或产品启动策略
```

`SceneAssetSerDeser` 遵循现有 Mesh/Texture SerDeser 形状，提供 `encodeData()`、`decodeData()`，并实现 `fromLuxAssetStream()`、`exportAsLuxAssetStream()`。不得在 `SceneAsset` 上添加静态 encode/decode。

### 2.3 Scene wire 合同

- 新 Scene Asset magic 为 `0x0130914D`。
- 外层使用标准 `AssetFileHeader`。
- data 区逐字节保存 LXSC v1；LXSC v1 的 magic、版本、字段顺序与数值宽度不变。
- 历史裸 LXSC magic `0x4353584C` 是同一 Scene descriptor 的只读 legacy magic。
- 裸 LXSC 可以解码并由内部 ID 合成 `AssetInfo`；再次导出只写新包裹格式。
- 外层 `AssetInfo::id` 与内层 Scene ID 必须非 nil 且相等。
- LXES v1 Section 是 Scene-owned Pak 内部记录，不注册为可驻留 Asset，不改变现有 content path。

### 2.4 Catalog 是唯一类型分派点

产品从标准 Resource descriptors 复制一份不可变 descriptor 列表，追加 `sceneAssetCodecDescriptor()` 后调用现有 `AssetCodecCatalog::build()`。

Catalog 负责：

- 按 type 查找；
- 按主 magic 或 legacy magic 查找；
- 拒绝重复 type、重复 magic、C++ type hash/name 冲突；
- 必要时通过 descriptor 的 image-to-shell 回调为无标准 Header 的 legacy image 创建 shell。

`AssetManager`、异步加载、Pak 验证和 Toolchain 分派统一使用 Catalog。Resource 不再维护 `assetTypeOfMagic()` 中央 switch，也不解释 Scene magic。

### 2.5 Provider 与 Pak 保持语义不透明

- `ProviderEntry` 暴露原始 `magic_number`，不把 Engine Scene 解释为 Resource enum 名称。
- `PakCookFileEntry` 直接接收 magic。
- Pak v2 的 `asset_magic` 字段及其字节格式不变。
- `RuntimeLaunchManifest` 必须显式给出 boot Scene 路径；Resource 不再提供“唯一 Scene 自动选择”的规则。

### 2.6 场景 Payload 由 ECS 领域拥有

以下类型、Codec 与契约测试从 Resource 二次归位到已有 ECS 领域 target，类型名、namespace 和 wire 不变：

| Payload | 最终 owner | Wire |
| --- | --- | --- |
| `TerrainTileBlobV1` / Codec | `ecs/terrain` | LXTT v1 |
| `TilemapChunkBlobV1` / Codec | `ecs/tilemap` | LXTC v1 |
| `StaticColliderBatch3DBlobV1` / Codec | `ecs/physics3d` | LXPC v1 |

这些 Payload 只有在对应 ECS 场景领域中才有意义，不属于通用 Description/Asset 类型目录。历史 `DESC-007/008/009` 表示当时 Resource 内容组件清零确已完成；本 ADR 不改写历史，而以 `SCENEASSET-*` 跟踪二次归位。

### 2.7 Runtime 与产品接入

- `engine/runtime/assets` 保留为现有 `AssetManager` 的异步编排适配器。
- Game、Editor 与相关 Toolchain 使用“标准 descriptors + Scene descriptor”的 immutable Catalog。
- Game 通过 VFS 和现有 `AssetLoadService` 加载 `SceneAsset`。
- `SceneRuntime` 消费 typed `SceneAsset` 并持有 `AssetRef`，不接收裸 LXSC image。
- Editor 临时场景也注册为 `SceneAsset`，但本 ADR 不重构 Editor 架构。
- GameExporter 输出包裹 SceneAsset，Section 继续输出裸 LXES。

## 3. 目录与 target

目标 Scene 目录：

```text
engine/scene/
├── CMakeLists.txt
├── include/lux/engine/scene/
│   ├── SceneDescription.hpp
│   ├── SceneAsset.hpp
│   ├── SceneAssetSerDeser.hpp
│   └── SceneFeatureId.hpp
├── src/
└── test/
```

单一 target/component 为 `lux::engine::scene::scene` / `scene`。旧 `scene_api`、`scene_package`、`ScenePackage`、`ScenePackageId`、旧 include 和 forwarding header 全部删除，不保留 alias。

## 4. 被取代的旧目标

以下 Checklist 项不再是待办，也不记为完成，统一标记“被 ADR-20260820 取代”：

- `ASSETSDK-001/002/005/016/017/018`
- `ENGEA-010..020`
- `SCENE-001`
- `SCENE-010`

其它使用 `AssetStore`、`AssetId`、`AssetTypeId` 或 `ScenePackage` 作为未来接口的章节，以本 ADR 为准解释；与本 ADR 冲突的示例不再具有施工约束力。

## 5. 不变边界

- 不推进 M0、全局 M7 命名迁移、Registry 重构、Editor 架构重构或 Extension ABI v4。
- 不重写 standalone Asio/stdexec/TBB 管线。
- 不改变 LXSC v1、LXES v1、LXTT v1、LXTC v1、LXPC v1 的内部字节。
- Classic Mesh、Texture、Mesh、Material 等通用内容保持当前 owner。

## 6. 验收原则

- legacy 裸 LXSC 可读，新格式 decode/re-encode 确定；新 data 区与 LXSC Golden 逐字节一致。
- Catalog 的 type/magic/C++ type 冲突和 legacy magic 路径有 owner tests。
- Resource 的生产代码、Pak 与 Provider 不再含 Scene 名称或 boot Scene 策略。
- 三类领域 Payload 在新 owner 下保持长度、SHA 与 decode/re-encode 契约。
- 安装包只暴露 canonical `scene` component；旧 Scene components 和 Resource 场景 Payload 头不存在。


---

# ADR：Asset 领域内聚、Pak 公共边界与 Engine Content

**状态：** Implemented

**日期：** 2026-08-21

**代码施工基线：** `f35e245a1e493c388722a41711f1a3ecd1df2acb`

**实施提交：** `1364810c`（owner/目录/API 迁移）、`e7348155`（wire/storage 契约与 v1 兼容修复）

**关联裁决：** `ADR-20260820_SceneAsset与Resource边界.md`

## 1. 背景

`modules/resource/asset` 已经收敛为单一公共组件，并保留了通用的
`LuxAsset`、`AssetManager`、`TAssetSerDeser`、Catalog、Provider、VFS 与 Pak。
当前源码仍按 `core/codecs/pak` 横向分区，导致一个资产族的值、SerDeser、私有
Description Codec、实现和测试被拆散；Engine Toolchain 还直接包含 Resource
`pinclude` 中的 Pak wire 实现。

Resource 同时公开 `BuiltinAssetIds.hpp`。这些冻结 UUID、`M_Missing` 与演示色板是
Lux Engine 默认内容契约，不是外部 Asset SDK 的通用语义。ECS Render 直接引用该头，
也使默认产品策略反向渗入 ECS。

## 2. 裁决

### 2.1 Asset 按领域族内聚

公共与私有源码按以下领域镜像组织：

```text
asset/
├── texture/   # Texture、TextureAtlas、FlipbookClip
├── material/  # Material、MaterialInstance
├── mesh/
├── model/
├── animation/ # Skeleton、AnimationClip
├── shader/
├── script/
└── storage/   # Provider、VFS、VirtualPath、Pak
```

`AssetCodecCatalog.hpp` 等跨领域资产核心合同保留在 Asset 根部。完整 `.luxasset`
转换统一使用 `*SerDeser`；被动值段转换继续使用私有 `*DescriptionCodec`。
`TextureCodec` 与 `ModelCodec` 分别改名为 `TextureSerDeser` 与 `ModelSerDeser`。
旧头、类型 alias、namespace alias 和 forwarding header 一律不保留。

### 2.2 Provider、VFS 与引用账本

Provider 是 `asset_id_t`/`VirtualPath` 到 opaque bytes 的存储接口，只负责
`resolve/contains/open/enumerate/pathOf`。它不创建 `LuxAsset`，不解释 Engine Scene
语义，也不修改资产引用计数。

```text
IAssetProvider -> AssetVfs -> AssetBlob
                              |-> Catalog/SerDeser -> AssetManager
                              `-> Scene Section Codec（不进入 AssetManager）
```

`AssetManager + AssetRef` 是唯一资产引用账本。Scene Section、Spatial 分区等记录可以
复用 Provider/VFS，但直接读取不产生 `AssetRef`，不参与驻留、revision 或驱逐。

### 2.3 Pak 是公共 Asset SDK 的存储格式

LUXPAK v2 的 Provider、writer 与 inspector 都属于 `modules/resource/asset/storage/pak`。
公共 API 提供 `PakWriteEntry`、`writePakFile()`、`PakInspectEntry`、`PakInspectInfo` 和
`inspectPak()`。Writer 只验证结构、路径、ID、magic、payload、排序、对齐、索引和
digest，不按 `EAssetType` 或 Catalog 解释条目语义，因此可以承载 Engine Scene 与
Scene Section 的原始 magic。

Engine Toolchain 只拥有 source 扫描、authoring auxiliary 剥离、Catalog/magic 策略、
冲突诊断、Scene/Game cook 组合与发布策略。Engine 不得包含 Resource `pinclude`，也不
得访问 `lux::asset::detail::Pak*`。

### 2.4 Engine 默认内容归 `engine/content`

新建轻量 `lux::engine::content` 组件，仅拥有冻结的内置资产 UUID、`M_Missing` 身份和
内置色板。它复用公共 `asset_id_t`，但不拥有 AssetManager、Codec、Provider、IO、异步
加载、几何生成或注册逻辑。

ECS 不依赖 Engine Content。`ResidencySubsystem` 接收可选的 fallback material ID：
Engine Runtime 在装配点注入 `builtinMissingMaterialId()`；通用 ECS/headless consumer
可以传 nil，明确禁用默认材质请求。Engine Toolchain builtin baker 继续拥有几何生成，
但其冻结身份来自 `engine/content`。

## 3. 公共结构

```text
modules/resource/asset/include/lux/engine/resource/asset/
├── Asset*.hpp
├── texture/
├── material/
├── mesh/
├── model/
├── animation/
├── shader/
├── script/
└── storage/
    ├── AssetProvider.hpp
    ├── AssetVfs.hpp
    ├── VirtualPath.hpp
    └── pak/
        ├── PakArchive.hpp
        └── PakAssetProvider.hpp

engine/content/include/lux/engine/content/
└── BuiltinAssetIds.hpp
```

单一 Resource target `lux::engine::resource::asset`、`lux::asset` namespace 与安装
component `asset` 保持不变。Engine Content target 为 `lux::engine::content`，安装到
`lux-engine-content` 的 `content` component。

## 4. 兼容性

以下字节合同不得改变：

- AssetFileHeader v1/v2；
- 所有标准资产内部 wire、magic 与 `EAssetType` 数值；
- 所有冻结内置 UUID 与颜色；
- LUXPAK v2 header、4 KiB B+tree page、16-byte payload alignment、SHA-256 与排序。

本 ADR 允许源码、target 依赖和安装头路径发生破坏性迁移，不允许格式升级或兼容 shim。

## 5. 验收

- 每个标准资产具备 deterministic length/SHA、decode/re-encode、legacy v1 与坏输入契约；
- Catalog 覆盖 type、main/legacy magic、C++ type hash/name 冲突；
- AssetRef 生命周期和 1->0 广播语义不变；
- Provider/VFS 的 opaque Section 读取不改变 AssetManager 账本；
- Resource 公共 Pak writer/inspector/provider 覆盖正常、冲突、截断与 digest 损坏；
- ECS fallback 的 nil/非 nil 两条路径均有 owner tests，ECS link closure 不含 Engine Content；
- installed consumer 可独立写、检查并读取 Pak；
- 安装树与全仓不存在旧 `codecs/`、旧 `pak/`、旧根部领域头、旧类名和 Resource
  `BuiltinAssetIds.hpp`。

## 6. 非目标

本轮不创建第二套 AssetStore、Provider、AssetId 或 AssetTypeId；不推进 M0、全局 M7
命名迁移、Scene/Registry/Editor 架构重写或 Extension ABI；不改变任何 wire/schema。


---

# ADR：Asset 运行期需求与 SerDeser 边界

**状态：** Implemented (`ed5fb7eb`)

**日期：** 2026-08-21

**代码施工基线：** `fe4422ba`

**关联裁决：** `ADR-20260820_SceneAsset与Resource边界.md`、`ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`

## 1. 问题

当前完整 `.luxasset` 已由各领域 `*SerDeser` 解析，但运行期异步加载又在
`AssetCodecDescriptor` 中维护 `decode`/`AssetDataInjector` 回调。Animation、Script 和
Editor Thumbnail 还分别注入 `AssetLoadFn`、同步 `ensureAsset()` 或 `ThumbnailLoadFn`。
同一件事因此存在“SerDeser 构造完整对象”和“裸数据回调填充 shell”两条链，类型、辅助
payload、错误与生命周期语义容易漂移。

## 2. 唯一加载链

运行期只保留以下链路：

```text
AssetClient
  -> AssetLoadService
  -> AssetVfs::open
  -> AssetCodecCatalog::decodeAsset
  -> concrete AssetSerDeser::parseLuxAssetMemory
  -> complete, unregistered LuxAsset
  -> AssetManager::installLoadedAsset
```

职责固定为：

- `AssetSerDeser/TAssetSerDeser` 是唯一具体 Codec 多态接口，只做 bounded、无副作用的纯解析；
- `AssetCodecCatalog` 只按 type、magic 与 C++ identity 选择 descriptor 并创建 manager-less
  SerDeser；
- `AssetLoadService` 负责编排阻塞 IO、后台解码、主线程安装、去重、retry/backoff、ABA 与 close；
- `AssetManager` 负责对象安装、AssetRef 账本、revision、事件与驱逐；
- `AssetRef` 只是稳定的驻留票据和 ID，不隐式触发 IO。

删除 descriptor 的 `AssetDataDecodeFn`、`AssetDataInjector` 和 `decode` 字段。Shell factory
只用于启动期轻量身份注册与 legacy Scene shell，不参与真实内容解码。

## 3. 安装语义

`AssetManager::installLoadedAsset(expected_id, decoded)` 在主线程安全点执行：

- 拒绝 null、nil/mismatched ID、type mismatch；
- 资产不存在时注册完整对象并保持 `on_registered`；
- data-less shell 原位替换为完整对象，不增加 content revision，不发送 content-changed；
- 已有完整对象时丢弃重复完成并返回现有对象；
- AssetRef 计数、revision 与异步 ABA 观察值不因 shell 替换而变化。

对象地址和 typed data 指针只保证使用到下一次 AssetManager 主线程 mutation/sync point；
跨帧身份必须使用 AssetRef/ID。热更新继续只走 `replaceAsset()`。

## 4. Runtime demand

ECS 不拥有加载编排，也不在 tick 中执行同步 IO：

- Flipbook/Skeletal Resolver 归 Engine Runtime presentation/animation pack，直接使用现有
  `AssetManager + AssetClient`；
- Script 的请求系统归 Engine Runtime Scene Script integration；ECS `ScriptSystem` 只消费
  已就绪资产；
- Thumbnail provider 只返回纯 `ThumbnailSpec` 与缺失依赖 ID，`ThumbnailService` 统一去重请求；
- 删除 `AssetLoadFn`、`ThumbnailLoadFn` 和 `syncTestLoader()`。

`ensureAsset()` 继续作为 Editor、Toolchain 和测试的显式同步 API，但 production ECS/Runtime
不得调用。Runtime pack 缺少 `SceneAssetServices` 时必须明确装配失败。

## 5. 完整对象合同

所有 manager-less decode 必须产生完整 owning `LuxAsset`：Script 保留 description、主 payload
与 auxiliary payload；Shader 保留 `ShaderInfo` 与 SPIR-V；Model 的运行期 manifest 即使没有
authoring node tree 也视为内容已就绪；Scene legacy LXSC 仍通过同一 descriptor 读取。

`LuxAsset` 成为独立多态基类，不继承 `LuxObject`。删除公开 `void* rawData()` 与模板
`LuxAsset::data<T>()`；只有具体 `TAsset<T>::data()` 暴露 typed pointer。

## 6. 兼容与非目标

AssetFileHeader v1/v2、各资产 wire、Scene legacy LXSC、Pak v2、magic、UUID 和 schema version
均不得改变。不增加 Loader 接口、第二套 AssetManager/Profile，也不让 AssetRef 自动加载。

`MODULES_SDK` 不是合法 Profile；Modules 边界由 installed consumers 在现有四个 Profile 的
安装结果上验证。


---

# ADR：Core Meta 纯化与 ECS Registry 归位

**状态：** Implemented (`ed5fb7eb`)

**日期：** 2026-08-21

**代码施工基线：** `fe4422ba`

## 1. 问题

`modules/core/meta/LuxObject.hpp` 同时包含 `LuxObject`、`EntityObject`、EnTT entity、Registry、
handle 与 allocator owner。结果是 Core Meta 和依赖它的 Serialization 安装闭包携带 EnTT，
而 Asset 只为继承一个空泛根类又依赖 Meta。这违背 `modules` 中 Core 不理解 ECS 运行时的
边界，也让反射生成器依赖虚构 OO 根类判断 record 是否可反射。

## 2. Registry owner

Registry 是 ECS 基础设施，统一归 `ecs/core`：

```cpp
lux::ecs::Entity
lux::ecs::kNullEntity
lux::ecs::RegistryBase
lux::ecs::Registry
lux::ecs::EntityHandle
lux::ecs::ConstEntityHandle
```

allocator、publication reservation/admission、snapshot、no-grow 与 handle 行为原样迁移。
删除 `lux::meta::EntityRegistry`、`entity_id`、`null_entity` 及旧头，不提供 alias、shim 或
forwarding header。ECS Core 建立自身 visibility API，并去掉仅由 `AssetLoadFn` 引入的
Resource Asset PUBLIC 依赖。

## 3. Core Meta contract

Core Meta 只拥有通用反射描述、查询和生成模型：

- 不 include、find 或 link EnTT；
- 不拥有 Registry、`LuxObject` 或 `EntityObject`；
- `LUX_CLASS/LUX_COMPONENT` 标注 record 直接是反射类型；
- 字段、参数和返回值通过生成模型中的 reflected-record identity 建立 `RefClass`；
- external 非侵入类型继续使用 `is_reflected_value_v<T>`；
- parent chain 只包含实际标注的反射基类，不注入虚构根类。

本 ADR 接受时曾允许 Core Serialization PUBLIC 使用通用 `RefClass/RefField`；该局部裁决已被
后续 `ADR-20260821_CoreSerialization与ECSComponentArchive边界.md` 取代。当前 Core
Serialization 只保留 byte Archive/NameTable，不再依赖 Meta 或 Eigen；反射归 ECS
`component_archive`。Asset 删除 Core Meta 依赖，Core Serialization 仅作为 Asset PRIVATE
实现依赖。

## 4. CMake 与验收边界

`modules/core`、`modules/platform`、`modules/function` 和 `modules/resource` 使用明确子目录列表，
不再自动枚举 production target。`description` 删除未使用的 Extension ABI 依赖。

验收至少包括：Registry allocator/publication/no-grow 回归；Schedule/Hierarchy/DeferredCommands/
PersistentEntity/Scene loading 回归；普通类、继承、字段/参数/返回值、external value 与 Lua
sidecar 反射；Core Meta/Serialization installed consumer 不获得 EnTT，ECS Core consumer 不
获得 Resource Asset，Asset consumer 不获得 Core Meta。

本 ADR 不推进 M0、M7、Extension ABI、VFS/Platform 迁移、Registry 产品级重写或 Scene Runtime
重写。


---

# Core Serialization 与 ECS Component Archive 边界

**状态：** Implemented

**日期：** 2026-08-21

**施工基线：** `6906ccc21bec1275fef8a45586b25b0337da2c4b`

**文档提交：** `d9d3619b`；**实施提交：** `d1ead288`

## 决议

`modules/core/serialization` 只拥有与领域无关的连续字节基础设施：

- `ArchiveReader` / `ArchiveWriter`；
- `NameTable`；
- `ByteReader` / `ByteWriter`；
- little-endian POD、字符串与 UUID 基础编码。

反射驱动的 Component tagged-property archive 归入 `ecs/serialization`，真实安装组件为
`lux::engine::ecs::component_archive`。它可以依赖 Core Meta 与 Eigen，但不得依赖
Resource Asset、Engine、Runtime、Editor 或 `entt::registry`。

旧 `TaggedPropertyArchive` 的 wire 不升级：全部 tag ordinal 保持不变，原源码名
`AssetRef = 48` 改为 `Uuid = 48`。UUID 的字节编码只说明值布局，不说明资产语义。
只有带非空 `asset_type=` reflection annotation 的 UUID 字段才是资产引用；字段名猜测
不再构成资产类型合同。

## API 与错误合同

Component Archive 使用详细 `expected` 失败结果。Failure 至少包含错误枚举、相对 byte
offset、field path 与 detail。Writer 在写入前完成 reflection/value/limit preflight；语义
失败不得留下半个对象或新增 NameTable 项。Reader 对失败对象不提供回滚，调用方必须丢弃
staging value。

compatible read 只允许“已知 Component 内的未知字段/未来未知非 Struct tag”被跳过；
坏 name index、重复已知字段、截断、嵌套越界、非法值和尾随字节必须失败。Cooked LXES
使用 exact read，字段必须按 schema 顺序恰好出现一次。

默认边界固定为：1 GiB object、16 MiB string、65536 fields/object、1M names、
4096-byte name 与 64 层嵌套。外层格式可以设置更严格 limits，但不得绕过这些边界。

## Registry 与 SceneFormat

不建立 `RegistryArchive`，也不把 `entt::registry` 作为文件镜像。持久化边界保持：

```text
Authoring/Toolchain Component payload
    -> ECS Component Archive
    -> ECS SceneFormat EntitySectionImage (LXES)
    -> Runtime EntityBatchStager
    -> ECS Registry
```

未知 Component schema 在 Authoring materialize、Toolchain cook 与 Runtime staging 均明确
失败；不能以跳过整个 Component 的方式继续。Extension schema 必须在 Section stage 前
已经提交到 `ComponentTypeCatalog`。

旧 Checklist 中的 `CORE-024 RegistryArchive` 与 `SectionMaterializer` 目标由上述现有边界
取代，而不是伪装成已完成。

## 兼容性

- LXWA v4 及子文档、World Descriptor Index v5、LXES v1、Persistence Journal v1、
  L3SC v1 与 Infinite2D payload 字节不变。
- 不保留旧 Core include、namespace alias、target alias 或 forwarding header。
- 本 ADR 不推进 M0、M7、Extension ABI、VFS/Platform、Scene Runtime 或 Editor 架构重构。

## 实施结果

Core Serialization 已删除 TaggedPropertyArchive、Meta 与 Eigen 闭包；ECS 已安装
`component_archive`。Authoring 使用 compatible reader，Cooked LXES 使用 exact reader，
三条 Unknown Component 路径均在 Registry 发布前失败。Core、ECS Component Archive 与
Function Animation installed consumers、四 Profile 全量构建及第二轮 no-op 已通过；证据见
`evidence/core-serialization-ecs-component-archive-6906ccc2.md`。


---

# Extension ABI v4 Owner 与 Core 清零

**状态：** Implemented

**日期：** 2026-08-21

**施工基线：** `2259ade725506f11fe247582d1c3be32116bebe0`

**文档裁决提交：** `2a916295`

**实施提交：** `c56efbc4`

## 背景

`engine/extensions/api` 已建立安装 target 和公共 include，但当前实现仍通过 forwarding
header 与 PUBLIC link 依赖 `modules/core/extension_abi`。因此它只是 Engine-owned 入口，
并未成为 ABI 定义的真实 owner；Checklist 中把该状态写成“已移动”是不准确的。

同时，Extension ABI v4 的注册回调直接接受
`RuntimeContributionRegistrar&` / `EditorContributionRegistrar&`。Extension DLL 会编译
registrar 的 inline 访问器并链接其 C++ 导出方法，因此在仍宣称兼容 ABI v4 时，不能只改
C++ 类型名而保持三个 `extern "C"` symbol string 不变。

## 决议

### 1. ABI 实体归 Engine

- `ExtensionId`、v4 descriptor/version/result、函数类型和 symbol constants 的唯一源码 owner
  迁入 `engine/extensions/api`。
- 保持 namespace `lux::extensions`、`kExtensionAbiV4 == 4`、descriptor 字段顺序、大小、
  对齐、枚举 ordinal、ABI fingerprint 与三个导出 symbol string 不变。
- `engine/extensions/api` 直接依赖所需 `lux-cxx::core/abi`，不再经过 Core component。
- 删除 `modules/core/extension_abi` 的源码、target、安装 component 和旧 include；不保留 alias、
  shim 或 forwarding header。

### 2. 删除通用 ContributionId

`ContributionId` 不属于 ABI v4 descriptor 布局，production 已全部使用领域 ID。删除
`ContributionIdTag/View/Id` 与 `contributionId()`，不建立替代通用 ID；Component、Scene
Feature、Render Effect 与 Extension 继续使用各自 owner 的 stable-name ID。

### 3. 冻结 ABI-facing 名称

在 ABI v4 生命周期内保留以下 ABI-facing 名称与对象布局：

- `RuntimeContributionRegistrar`
- `EditorContributionRegistrar`
- registrar 公共头中参与对象布局或 inline 访问的 lease/draft 类型
- `ExtensionModuleDescriptorV4`、`EExtensionModuleTarget`、
  `ExtensionRegistrationResult` 与 `EExtensionRegistrationError`

因此旧 Checklist 中把 ABI-facing registrar/draft/lease 直接改名的目标由本 ADR 取代。
`ExtensionModuleManager` 等纯宿主内部类型仍可在后续独立迁移中改名为 Loader，但不得借此
改变 v4 plugin surface。

### 4. Reflection 边界

现有 `ReflectionRegistrationDraft` 已提供 validate-before-publish、commit 与 rollback，并由
Extension 装配链使用。本轮保留该事务语义。删除静态 pending registrar 链需要显式 codegen/
plugin entry contract，属于后续 ABI/codegen ADR；不得在 v4 owner 搬迁中暗中增加新入口或
伪装成纯重命名。

## 本轮范围

本轮完成：

- `CORE-002/003/005/006/010`
- `EXT-007/012/025/026`
- `FINAL-001`
- 新增 `EXTABI-*` 证据条目

本轮不推进 `EXT-002..005`、`EXT-013..024`、`CORE-016..018`、`ECS-028`、Platform/Render、
M0、M7、Scene Runtime、Game/Editor 架构或 Extension ABI v5。

## 验收合同

- ABI v4 layout/ordinal/fingerprint/symbol contract 与现有 Extension DLL fixture 通过。
- Engine Extension SDK installed consumer 可只查找 `lux-engine-extensions COMPONENTS extension_api`。
- `lux-engine-core COMPONENTS extension_abi` 必须失败，三个安装前缀不含旧头或旧 export。
- production/test/CMake 不存在旧 Core include、target/component 或通用 `ContributionId`。
- 四个 Profile 全量构建和 owner tests 通过，CMake 第二轮为 no-op。

## 实施结论

`c56efbc4` 已完成 owner 搬迁与旧 Core component 清零。Authoring Project Manifest 保持
Authoring-owned source DTO，不反向依赖 Runtime 产品；Game Exporter 与 Editor
ProjectController 在 authoring→cooked/runtime 边界显式转换为 Engine `ExtensionId` 与 target。
Runtime、Scene、Game、Editor Extension API 和实际 Extension DLL 均只消费
`engine/extensions/api`。

Windows x64 v4 descriptor/result 的 size、alignment、field offset、enum ordinal、fingerprint
和三个 symbol string 已由 owner contract 固定；动态 fixture 覆盖 dependency、rollback、
reflection publication、lease/unload 与 Editor-only registration。实际 Physics2D DLL 导出
`luxGetExtensionModuleV4` 与 `luxRegisterRuntimeContributionsV4`。完整验证见
`evidence/extension-abi-core-retirement-2259ade7.md`。


---

# ADR-20260821：GAPI 保留裁决

## 状态

ACCEPTED，取代执行文档中所有“把 `modules/platform/gapi` 并入 Render Vulkan 并删除旧 owner”的目标。

## 决策

`modules/platform/gapi` 保持为 Modules SDK 中公开、可独立使用的 Platform 图形 API 组件：

- target、安装 component、公共 include 与 `lux::engine::platform::gapi` namespace 均保留；
- GAPI 继续拥有其低层 Vulkan wrapper 与对象生命周期合同；
- Render Vulkan 可以依赖 GAPI，但不取得 GAPI 的所有权；
- 本重构不强制把 GAPI wrapper 与 Render 内部 handle/资源模型合并为一种类型；
- 不创建迁移 alias、forwarding header 或替代 target。

GAPI 与 Render 的职责不同：GAPI 是外部项目也可直接使用的低层图形 API，Render Vulkan 是引擎 Render 协议的一种实现。二者使用同一 Vulkan 后端不等价于二者必须共享同一公共对象模型。

## 被取代的施工目标

以下 Checklist 项保持未勾选，并标记为 `SUPERSEDED`：

- `PLATFORM-009/010/011`
- `RENDER-009/010`
- `FINAL-003`

迁移映射 `modules/platform/gapi -> modules/function/render/vulkan/low_level` 改为 `KEEP`。阶段退出条件与最终验收不得再要求 GAPI 目录、target、component 或 namespace 消失。

## 非目标

- 本裁决不禁止 GAPI 内部修复、测试或面向 Vulkan 版本的正常演进。
- 本裁决不修改 Window/Vulkan Surface 的所有权；该边界仍由后续 Window/Render Surface 阶段处理。
- 本裁决不改变任何 GAPI production 代码或安装 ABI。


---

# ADR-20260821：单体 Input 子系统边界

## 状态

ACCEPTED，取代“为每个窗口后端建立公开 Input Adapter target”的旧目标。

## 决策

Input 只提供一个公开 target 与安装 component：

```text
lux::engine::function::input
```

公开的 `lux::input::Input` 是完整输入领域对象，拥有：

- 当前帧 `InputSnapshot`；
- `ActionMapper`；
- `ActionMapper` 内唯一的 `InputActionRegistry`；
- `InputContextStack`。

Window 只采集 OS/window backend 的原始 key、mouse、scroll 与 text 事件。`Input::sample(window)` 读取并规范化这些事件，`Input::evaluate()` 再执行 Action Mapping。采集与求值刻意分成两个阶段，使 Editor 可以在采集后依据 ImGui/Viewport 状态决定 keyboard/pointer routing。

平台差异只存在于同一 target 的私有实现：CMake 在配置时选择 `src/platform/glfw/InputPlatform.cpp` 或 `src/platform/android/InputPlatform.cpp`。不得增加：

- `input_glfw` / `input_android` target 或 component；
- `InputAdapter` / `IInputBackend`；
- backend factory、catalog、注册表或运行期多态；
- 必需依赖 setter、hook 或 Service Locator。

## 所有权与依赖

- normalized physical input、snapshot、Action/Context 语义属于 Function Input。
- Platform Window 拥有原始事件 acquisition 与 per-window event queue，但不解释 Action、Context、UI capture 或 held/edge 状态。
- Input 对 Window/GLFW 的构建链接为 PRIVATE；Input 公共头不 include GLFW。
- `ActionMapper` 继续是同一 target 内可独立使用的算法类，服务于 headless Preview、测试和低层 Runtime。
- GameApplication 不再维护第二份 `InputActionRegistry`；脚本名称查询与 Mapper state 必须使用同一 Registry。

## 兼容性

本阶段允许源码与安装头路径破坏，不保留旧 `lux::window` 输入类型、旧头或 forwarding alias。输入类型没有持久化 wire；Scene、Asset、Pak 与 Authoring 字节格式不受影响。

## 非目标

- 不实现真实 Android GameActivity 输入；Android 当前确定性地产生空快照并保持可编译。
- 不完整拆分 Window core/backend、Vulkan Surface、Tray 或 UI。
- 不引入输入设备热插拔、gamepad、重放文件格式或新的线程模型。


---

# Asset 领域内聚基线与 wire 验收证据

**基线：** `f35e245a1e493c388722a41711f1a3ecd1df2acb`

**实施：** `1364810c`、`e7348155`

## 基线公共面

- `modules/resource/asset/include` 共 32 个安装头。
- 唯一库/target/component 已是 `lux_engine_asset` / `lux::engine::resource::asset` / `asset`。
- 基线仍包含根部领域头、`codecs/`、`pak/`、`BuiltinAssetIds.hpp` 和 Toolchain 对 Pak `pinclude` 的越界访问；实施后均归零。
- 安装导出仍只有 Resource `asset` component；新增 Engine Content 作为独立 `lux-engine-content/content` component，不改变 Resource component 身份。

## 冻结 wire 指纹

下列 fixture 由确定性输入生成；迁移后验证当前格式 decode/re-encode 逐字节一致，并验证历史 AssetFileHeader v1 可读。任何有意变更必须先升级格式版本。

| Fixture | 字节数 | SHA-256 |
| --- | ---: | --- |
| texture | 820 | `e01de6ccfb600f997b0ad08035acbda1c404647faa86284e4dcd28a03efed3cc` |
| texture_atlas | 477 | `2c0a7f6353760c6994065c143b169707c16191899076b53c0814604e5a86d2e1` |
| flipbook_clip | 469 | `38e7fa62a043f95947ba06b0a756118ec86ea33250195791038e541747a15533` |
| material | 495 | `34ddba8c3a78463d048553fa3d44481a737646895b6797e8fb62e19e9bd1fd8f` |
| material_instance | 472 | `a35ae4037601ccabd656b458ce979beec7947ccb3081345ddb3955fabbb6d495` |
| mesh | 708 | `55d3667e298f4b5a358cdd9979b348323d5c911ff5f5971f55beeaf181b5f765` |
| model | 485 | `715aa44f5c17fb9fcf23fb2f91bc35b2c4b9db084ba6325ce5dcfc92822558be` |
| script | 531 | `0c9673e7e98a1aa11c027658ab12d4e7f42d75c8a5885ef89453d9836cc886ed` |
| shader | 423 | `ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d` |
| skeleton | 576 | `0f9757141b0f49ac269a74901050c96d378d71858227e31b58e6aeca0ece0248` |
| animation_clip | 496 | `c88929b5122c40953854b8828d48b87022a7711b8705e249d2df7145ef0baf50` |
| LUXPAK v2 / 1000 entries | 163840 | `18b617f54954c5ec5548c8f38b460603459c03bdf50ea598815c3791edf5e715` |

## 验收结论

- Asset/Pak 格式、magic、UUID 排序、16 字节 payload 对齐、4 KiB B+tree page 与 SHA-256 行为未改变。
- 版本化 header 校验按 v1/v2 实际 header 大小执行；修复前被误拒绝的 v1 image 现可读取，v2 fixture 指纹不变。
- Public Pak writer/inspector/provider、opaque VFS、AssetRef 账本、Catalog 冲突、Engine Content UUID 与 ECS fallback 注入契约均通过。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的 Windows RelWithDebInfo `target all -j 4 -k 0` 通过，第二轮均为 `ninja: no work to do`。


---

# Asset Pipeline、Runtime Demand 与 Core Meta 验收证据

**施工基线：** `fe4422ba`

**实现提交：** `ed5fb7eb`

**验收日期：** 2026-08-21

## 边界结果

- `AssetCodecCatalog::decodeAsset()` 通过 manager-less `AssetSerDeser` 返回完整 owning `LuxAsset`；descriptor 的 decode/injector 回调已删除。
- 同步 `AssetManager::ensureAsset()` 与异步 `AssetLoadService` 共用 decode/install 路径；shell 填充保持 AssetRef 账本、revision 与事件合同。
- Animation Resolver 与 Script request system 归 Engine Runtime integration；ECS production 与 Runtime production 均不存在同步 `ensureAsset()`。
- Thumbnail provider 只报告缺失依赖，ThumbnailService 通过既有 AssetClient 去重请求。
- Registry、allocator、Entity 与 handles 归 ECS Core；Core Meta 删除 EnTT、Registry、LuxObject 与 EntityObject。
- Asset installed consumer 不导入 Core Meta；Core Meta/Serialization installed consumer 不导入 EnTT；ECS Core installed consumer 不导入 Resource Asset。

## 契约测试

- Asset：Catalog、11 类 wire Golden、legacy v1、lifecycle、shell install/reload、AssetLoadService dedup/retry/ABA/close 全部通过。
- Scene：SceneAsset legacy LXSC、新包裹格式与 data 区 Golden 契约通过。
- Runtime demand：Flipbook2D、SkeletalAnimation、Script request system 的已加载/延迟加载/顺序契约通过。
- ECS/Reflection：Registry capacity/publication、Schedule、ComponentTypeCatalog、Entity Section loading、Reflection drain、Spatial external reflection 与 Lua sidecar 回归通过。
- Editor：Thumbnail payload、scene roundtrip 与相关 owner 回归通过。

## 构建与安装

- Windows x64/MSVC/RelWithDebInfo 的 DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 均完成全量 `target all -j 4 -k 0`；各构建树第二轮均为 `ninja: no work to do`。
- 四个构建树均执行 CTest 并成功退出；工程当前注册 0 项 CTest，契约测试因此按 owner 可执行文件直接运行。
- Debug、RelWithDebInfo、Android 三个安装前缀的变更公共头已同步；五个退役头已精确删除。
- Asset、Core Meta/Serialization 与 ECS Core installed consumers 均完成配置、编译、链接和运行。
- 旧回调、旧 Registry/OO 根类、旧 Resolver 公共头及禁止依赖闭包扫描归零；`git diff --check` 通过。


---

# Core Serialization / ECS Component Archive 施工证据

**施工基线：** `6906ccc2`
**文档提交：** `d9d3619b`
**实施提交：** `d1ead288`
**日期：** 2026-08-21

## 所有权与安装闭包

- Core Serialization 只导出 Archive、NameTable、UUID/POD/string 与 bounded `readSpan()`；导出链接闭包为 `lux::cxx::binary;stduuid`，不含 Meta、Eigen、ECS 或 Engine。
- `lux::engine::ecs::component_archive` 独立安装，PUBLIC 依赖 Core Serialization、Core Meta、compile_time，Eigen 为 PRIVATE；不含 Resource、Runtime 或 Engine。
- Function Animation 的导出闭包为 Core Math + Resource Description，不再查找 Resource Asset。
- Debug、RelWithDebInfo、Android 三个 include 前缀均含新 ECS 头且不含旧 Core TaggedPropertyArchive 头。

## 格式与行为契约

- `EArchiveType` ordinal 保持不变；源码名 `AssetRef=48` 改为 `Uuid=48`，fixture 仍为 60 bytes，tag byte 仍为 48。
- Component Archive owner test 覆盖 preflight 无副作用、详细错误、limits、compatible future field、exact canonical schema、重复/缺失/类型漂移、截断/尾随、NaN、UUID annotation 与 nested payload 边界。
- `world_source_codec_test` 继续固定 LXWA v4 与全部子文档 length/SHA；SceneFormat/LXES/Persistence、Spatial3D L3SC、Infinite2D 与 Editor roundtrip owner tests 均通过。
- Unknown Component schema 在 Authoring、Toolchain、Runtime 三条路径明确失败；Authoring/Runtime 断言 Registry 在失败前未发布部分状态。

## 构建与消费者

| 验证 | 结果 |
| --- | --- |
| DEVELOPER RelWithDebInfo `target all -j 4 -k 0` | PASS；第二轮 `ninja: no work to do` |
| PLAYER RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| EDITOR RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| TOOLCHAIN RelWithDebInfo | PASS；owner contracts PASS；第二轮 no-op |
| Core Serialization installed consumer | configure/build/run PASS；无 Meta/Eigen/ECS/Engine |
| ECS Component Archive installed consumer | configure/build/run PASS；无 Resource/Runtime/Editor |
| Function Animation installed consumer | configure/build/run PASS；无 Resource Asset |
| module layout / target DAG | 四 Profile 配置通过 |

四个构建树当前均报告 `No tests were found!!!`，因此验收记录来自独立 owner test executables，未把 0 项 CTest 误报为测试覆盖。旧构建目录中的 `entity_scene_contract_test.exe` 是已删除 Resource target 的历史残留，不属于当前 CMake DAG；现行 LXES 契约由 `entity_section_wire_compatibility_test`、`entity_scene_cooker_test` 与 `runtime_entity_scene_integration_test` 验证。


---

# Extension ABI v4 Owner / Core 清零施工证据

**施工基线：** `2259ade7`
**文档裁决提交：** `2a916295`
**实施提交：** `c56efbc4`
**日期：** 2026-08-21

## Owner 与依赖边界

- `engine/extensions/api` 直接拥有 ExtensionId、descriptor/version/result、函数签名与三个 v4 symbol constants；installed target 的 `INTERFACE_LINK_LIBRARIES` 只有 `lux::cxx::core;lux::cxx::abi`。
- `modules/core/extension_abi` 的目录、target、component、旧 include 和安装 export 已删除；Core available components 仅为 math/meta/serialization/log/events。
- 通用 `ContributionIdTag/View/Id` 与 `contributionId()` 已删除；production、test 和 CMake 精确词扫描归零，不建立替代通用 ID。
- Authoring Project Manifest 使用 Authoring-owned extension source DTO；Game Exporter 与 Editor ProjectController 在 authoring→cooked/runtime 边界显式转换为 Engine Extension API。

## ABI v4 契约

- `kExtensionAbiV4 == 4`，ABI fingerprint 与 `lux::cxx::AbiBuildInfo::fingerprint()` 一致。
- Windows x64 固定 `ExtensionDependencyView` 为 24 bytes/alignment 8，`ExtensionModuleDescriptorV4` 为 80 bytes/alignment 8，关键 offset 为 target=62、dependencies=64、dependency_count=72；registration result 为 1 byte/alignment 1。
- `EExtensionModuleTarget` ordinal 固定 0/1；`EExtensionRegistrationError` 固定 0..7。
- 三个导出字符串保持 `luxGetExtensionModuleV4`、`luxRegisterRuntimeContributionsV4`、`luxRegisterEditorContributionsV4`。
- 动态 fixture 验证 path/memory image loading、dependency closure、validate-before-publish、component reflection commit/rollback、duplicate rejection、lease 与 unload；Editor-only registrar transaction 通过。
- 实际 `org.lux.physics2d.runtime.dll` 由 `dumpbin /exports` 确认导出 module/runtime 两个 v4 C symbols，Game Export extension smoke 通过。

## 构建、安装与消费者

| 验证 | 结果 |
| --- | --- |
| DEVELOPER RelWithDebInfo `target all -j 4 -k 0` | PASS；公共头最终重编 144/144；第二轮 no-op |
| PLAYER RelWithDebInfo | PASS 157/157；第二轮 no-op |
| EDITOR RelWithDebInfo | PASS 220/220；第二轮 no-op |
| TOOLCHAIN RelWithDebInfo | PASS 44/44；第二轮 no-op |
| Engine Extension API installed consumer | configure/build/run PASS；六个公共头可共同消费 |
| 旧 Core component 反向查找 | 按预期失败：`extension_abi` 不在 available components |
| Debug/RelWithDebInfo/Android include 前缀 | 新六头 SHA 与 source 一致；旧 Core ABI 头不存在 |
| RelWithDebInfo package exports | 新 `lux-engine-extensions/extension_api` 存在；旧 Core export 不存在 |
| module layout / target DAG | 四 Profile 配置通过；旧 target 不在 classified/unclassified graph |

四个构建树的 CTest 命令均成功执行，但工程当前注册 0 项；因此本证据明确来自 owner executables，未把空 CTest 当成覆盖。
