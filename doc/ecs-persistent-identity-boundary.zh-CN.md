# ECS 持久实体身份边界

## 决议

`PersistentEntityId` 与 `PersistentEntityRef` 是 ECS 运行时身份，不属于 LXSC/LXES 文件格式。

运行时唯一事实源为：

```cpp
lux::ecs::PersistentEntityId
lux::ecs::PersistentEntityRef
lux::ecs::PersistentEntityIndex
```

历史类型：

```cpp
lux::entity_scene::PersistentEntityId
lux::entity_scene::PersistentEntityRef
```

仅表示冻结的 LXSC/LXES v1 数据传输对象。二者必须保持强类型隔离，不能增加隐式转换构造函数。

## 合法依赖方向

```text
Legacy LXSC/LXES DTO
        ↓ 显式 value() 转换
ECS PersistentEntityId / PersistentEntityRef
        ↓
PersistentEntityIndex 与运行时 Component/System
```

反向写入 Authoring 或旧格式时同样执行显式转换：

```cpp
lux::entity_scene::PersistentEntityId{runtime_id.value()}
```

转换只能位于明确边界，例如：

- Entity Section staging/materialization；
- `WorldActorEcsAdapter`；
- Legacy Scene Package Adapter；
- 字节兼容测试。

## ECS Kernel 禁止项

`ecs/core` 不得依赖：

- `modules/resource/entity_scene`；
- `lux::entity_scene::*`；
- LXSC/LXES Manifest 或 Codec；
- Engine Scene Package。

ECS Kernel 只拥有实体身份、索引及其一致性协议。磁盘格式继续由 `ecs/scene_format` 与 Engine Scene Package 分层拥有。

## 运行时发布不变量

1. wire ID 在 staging 边界转换成 ECS ID；
2. `PersistentEntityIndex` 只接收 ECS ID；
3. Component 反射字段使用 `lux::ecs::PersistentEntityRef`；
4. 已武装批次通过 Claim 预留 ID，命令屏障发布时原子提交；
5. 普通实体不自动获得持久 ID；
6. Legacy UUID 包装类型不得进入 ECS Component 或 System API。

## 自动门禁

```text
tools/architecture/check_persistent_entity_identity.py
```

门禁扫描 `ecs/`、`engine/runtime/` 与 `ecs/core` CMake 闭包。只有旧 wire 字节兼容测试允许同时引用两类身份，以证明：

- 类型不可隐式互换；
- UUID 值可以在显式 Adapter 中无损转换；
- LXSC/LXES 编码字节保持不变。
