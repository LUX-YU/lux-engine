# Core Serialization 与 ECS Component Archive 边界

**状态：** Accepted，代码施工待完成

**日期：** 2026-08-21

**施工基线：** `6906ccc21bec1275fef8a45586b25b0337da2c4b`

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
