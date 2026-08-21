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
