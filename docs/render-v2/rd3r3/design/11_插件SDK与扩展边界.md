# 11 — 可安装 SDK、插件 Feature、DataProjection 与 CodePin

## 11.1 一条扩展路径，不改核心业务 switch

插件可贡献：neutral DataSchema/PassSchema；RenderFeature 作者配置与构建 Ops；Engine-side DataProjection（观察自己的 ECS Component 或 Editor 数据）；可选 Native storage Factory/特殊 Shader/DeviceCaps fallback；版本化 Operation/Widget 数据。宿主将这些注册为完整的、冷态验证的结构合同，新的 Feature 和新数据类型无需修改 Renderer 的 `switch(FeatureType)` 或硬编码 Proxy 表。

## 11.2 DeviceSession 不会泄漏到 Engine

Feature build 在 neutral graph/requirements 上操作，不持 Vulkan Device；在真正设备专属 Native Prepare 或资源 Factory 时才由已选 Native Backend 取得窄的 `DeviceContext`/`ResourceContext`，必要时可借 DeviceSession，但不绑定 Scene A 的 Storage 指针到可跨场景的 Effect author。Engine plugin 的 Projection 只依赖中立 RenderRuntime/Schema/Transport，不能 include Vk/WSI/Native private headers。

## 11.3 ABI 纪律

跨 DLL 的 C ABI 必须使用版本化结构 size/capabilities、Host-owned metadata、opaque handles、callback table，不跨 ABI transfer owning STL/RTTI/exceptions。为任何回调、注册 Schema、Native shader kernel、资源析构代码设置真实 CodePin，直到所有调用及对应 GPU/CPU 生命周期结束才允许卸载。Host 会验证 data type canonical name/version/Field ABI，冲突返回错误而不是注册顺序优先。

## 11.4 Installed 插件用例

安装后独立 consumer 新增自有 `GridComponent`、`GridData`、`GridFeature`、`GridProjection`（或 Gaussian 数据类型），无需修改 render backend 源码；真实 ECS 变化触发 typed wire publication、ScopeStorage 更新、attach GPU output readback。插件卸载前创建仍在用的 Effect/Attachment/Native Code/Resource Destruction，卸载后不存在可调用的 DLL 代码悬挂。外部编译与运行要实际测试，不以 Header-only 示例冒充 SDK PASS。

## 11.5 数据发布有权威

同一 DataSchema 允许不同 Producer 类型（ECS、Editor UI、外部 robot source），但**同一具体 scoped data key**默认一个 writer；多个 writer 需显式 partition/merge/priority transaction，不能默默采用最后到达的包。Project/Scene 生命周期变化会撤销 Producer 关联和相关 Attachment，而不是再注册一套永久监听器。
