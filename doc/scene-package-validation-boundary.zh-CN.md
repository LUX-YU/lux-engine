# ScenePackage 原生验证边界

## 目的

`lux::scene::ScenePackage` 是 Engine Scene 领域的规范内存模型。旧
`modules/resource/entity_scene` 只保留 LXSC v1 字节兼容职责，不能继续
作为 ScenePackage 的模型验证器。

## 当前分工

- `ScenePackageValidation.cpp` 直接验证 Engine-owned 类型：
  `ScenePackageId`、`SceneFeatureId`、ECS `EntitySectionId`、Extension
  requirement、Component schema requirement、Section dependency graph 与
  startup policy。
- `ScenePackageCodec.cpp` 暂时仍通过 `LegacyEntitySceneAdapter` 复用 LXSC v1
  encoder/decoder，以保证本阶段不改变字节格式；编码前与解码转换后都必须
  经过 canonical `validateScenePackage()`。
- `LegacyEntitySceneAdapter` 只负责新旧模型转换和 codec error 映射；它不再
  是 `validateScenePackage()` 的实现入口。

## 不变量

1. ScenePackage 验证不得 include 或构造 `lux::entity_scene` 数据传输对象。
2. Stored Section path 必须通过 Asset `VirtualPath` 的唯一 parser；不得在
   Scene 领域复制路径 grammar。
3. Feature、Extension、Component、Demand Channel 与 Generator 的 ID 必须由
   各自领域的验证函数检查。
4. Section、startup list、requirements 与 feature list 必须保持 wire 所需的
   canonical ordering，且不得有重复项。
5. Section dependency 必须引用同一 Package 内的 Section，且图必须无环。
6. 目录/所有权迁移不得与 LXSC wire version 变化合并。

## 验证目标

- `scene_package_validation_test` 只链接 Engine `scene_package`，覆盖合法模型、
  非法 VirtualPath、重复 Feature、缺失 startup Section 与依赖环。
- `scene_package_contract_test` 继续链接冻结的 legacy component，只负责新旧
  LXSC v1 编码逐字节兼容和交叉解码。
- `scene_package_public_link_test` 验证共享库导出与安装链接闭包。
- `check_entity_section_boundary.py` 阻止验证逻辑重新进入 wire adapter，或
  canonical validation test 重新 include legacy Resource 模型。

## 后续删除闸门

只有当 `encodeScenePackage()` 与 `decodeScenePackage()` 也拥有原生 LXSC v1
实现、Golden Files 通过，并且所有 legacy compatibility tests 独立保留后，
`scene_package` 才能删除对 `resource/entity_scene` 的 PRIVATE 依赖。

Asset `VirtualPath` 目前仍位于混合的 `asset_core` target；下一阶段应将其与
Provider/VFS contract 抽为 Asset SDK 叶子组件，再收窄 ScenePackage 的私有
链接闭包。
