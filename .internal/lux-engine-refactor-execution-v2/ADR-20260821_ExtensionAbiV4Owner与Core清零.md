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
