# LUX Engine 架构重构执行文档 v2.0

**代码基线：** `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656`
**Scene Asset 实施提交：** `36ce56c634f1e03959910fc1761ec62b2fb4671e`
**当前施工基线：** `f35e245a1e493c388722a41711f1a3ecd1df2acb`
**文档日期：** 2026-08-21

本套文档替代此前 v1.x 文档。新版以以下哲学为中心：

```text
modules = 可独立分发的公共 SDK（含通用 AssetManager/Codec/VFS/Pak）
ecs     = Entity/Component/World/Schedule 与 ECS 领域 Payload
engine  = Scene/Execution/Extension/Game/Editor 领域
products= 最终入口与可执行程序
```

关键修正：

- 不创建 `engine/module/Modules.hpp` 或通用 Module Runtime。
- `Extension`、Library Module、CMake Module、`ScriptModule` 严格分离。
- `modules/resource` 只保留 `description` 与 `asset` 两个一级语义。
- Extension ABI、Game Manifest、Entity Scene、World Partition 等回到真实上层。
- 保留公共 `AssetManager`、`LuxAsset`、Codec、Provider、VFS 与 Pak，不建立第二套 Engine AssetStore。
- Engine-owned Scene 复用公共资产机制；场景 Payload 由对应 ECS 领域拥有。
- Asset 按领域族内聚；Provider/VFS/Pak 是 opaque bytes 存储面，Pak 读写检查属公共 SDK。
- 冻结的 Engine 内置资产身份归 `engine/content`，ECS fallback 由 Engine Runtime 装配注入。
- 先纯化公共 Modules 和 ECS 依赖方向，再实施 Game/Editor 重构。

## 文档目录

| 编号 | 文档 | 内容 | 文件 |
| --- | --- | --- | --- |
| 00 | 架构宪章与变更控制 | 不可变决议、层级、命名、所有权和执行顺序 | 00_架构宪章与变更控制.md |
| 01 | modules 公共 SDK 边界 | 公共库准入、安装包、隔离 Profile 与架构扫描 | 01_modules公共SDK边界与分发体系重构.md |
| 02 | Core 与 Platform | extension_abi/meta/common/gapi/window 清理 | 02_Core与Platform基础库清理.md |
| 03 | Resource | Description/Asset 两级语义、既有 AssetManager、Codec、Provider 与 VFS/Pak | 03_Resource-Description与Asset重构.md |
| 04 | Function | Render/Input/Animation/Navigation/Script/UI 公共闭包纯化 | 04_Function公共模块重构.md |
| 05 | ECS | Registry、ComponentSchema、Serialization、Scene Format | 05_ECS内核-序列化与SceneFormat重构.md |
| 06 | Engine | Executor、Scene Asset、ExtensionLoader、Render Bridge、关闭协议 | 06_Engine执行-资产-场景与扩展重构.md |
| 07 | Game 与 Editor 产品 | 共享 Session、删除 Host、Game Manifest 与导出语义 | 07_Game-Editor与共享Session产品重构.md |
| 08 | Editor | Workspace、Workbench、Documents、Panels、Flow/Material/Script/Preview | 08_Editor-Workspace-Workbench-Documents-Panels重构.md |
| 09 | CMake 与 SDK | Target、Namespace、Package、Compatibility、Profile | 09_CMake-命名空间-SDK包与兼容迁移.md |
| 10 | 测试与验收 | 持续集成、Sanitizer、Golden、故障注入、PR 路线 | 10_测试-CI-PR路线与验收.md |
| 11 | 施工 Checklist | 625 个可勾选施工项 | 11_详细施工Checklist_已更新_20260819_b1a25d3.md |
| 12 | 迁移映射总表 | 127 条当前→目标映射 | 12_迁移映射总表.md |
| 13 | 当前代码事实索引 | 69 个关键文件锚点 | 13_当前代码事实索引.md |

补充 ADR：`ADR-20260820_SceneAsset与Resource边界.md`。它取代旧文档中关于
`AssetId`、`AssetTypeId`、`engine/assets/AssetStore` 与 Resource 场景 Payload 最终归属的目标；
冲突内容只保留为历史设计记录，不再作为施工要求。

`ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md` 进一步裁决 Asset 目录、Storage/Pak 公共边界、Engine Content owner 与 ECS fallback 注入。

## 推荐阅读与施工顺序

```text
00
 ↓
01 → 02 → 03 → 04
               ↓
              05
               ↓
              06
               ↓
           07 → 08
               ↓
              09
               ↓
              10

11 全程勾选
12 全程更新
13 用于定位当前代码
```

## 最先执行的五个 Pull Request

1. 增加 `MODULES_SDK` Profile 与公共依赖闸门。
2. 停止 modules 子目录自动枚举。
3. 将 `extension_abi` 从 Core 安装闭包中隔离，建立 Engine Extension API 新目标。
4. 解散 `platform/common`，把依赖改为精确目标。
5. 将 `resource/spatial` 的纯值迁入 Math。

这五步先建立防回归边界，不立即改产品行为。

## 文件说明

- `00-13_全部执行文档与Checklist_合订本.md`：单文件全文。
- `11_详细施工Checklist_已更新_20260819_b1a25d3.md`：项目管理与 Pull Request 跟踪用。
- `12_迁移映射总表.md`：路径/类型/Target 映射。
- `13_当前代码事实索引.md`：固定提交上的代码锚点。
- `ADR-20260820_SceneAsset与Resource边界.md`：Scene Asset 与公共 Resource 的现行裁决。
- `ADR-20260821_Asset领域内聚-Pak边界与EngineContent.md`：Asset 领域内聚、Pak 和 Engine Content 的现行裁决。
- `SHA256SUMS.txt`：校验值。
- `manifest.json`：机器可读文件清单。

## 状态说明

这些文档同时记录施工规范与已验证状态。`SCENEASSET-001..020` 已按 ADR 完成；其余未勾选目标仍须在后续变更中引用 Checklist ID，并同步更新映射表和事实索引。
