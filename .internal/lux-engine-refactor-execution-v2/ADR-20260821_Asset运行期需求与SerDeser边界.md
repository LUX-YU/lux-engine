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
