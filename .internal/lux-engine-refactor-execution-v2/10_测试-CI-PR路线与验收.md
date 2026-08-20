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

退出：Extension ABI、gapi、common、spatial 归位。

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
