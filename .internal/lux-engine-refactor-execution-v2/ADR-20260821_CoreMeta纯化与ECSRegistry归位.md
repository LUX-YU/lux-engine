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
