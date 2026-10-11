# 11 — Public Feature/Data SDK、插件、ABI 与权限

## 11.1 公开两层接口、唯一运行时目录

**C++20 作者接口**：强类型 `DataSchema<T>`, `Reader<T>/Writer<T>`, `DataSetRef<T>`, `GpuDataView<T>`, `PassSchema<P>`, `RenderGraphBuilder`, Feature Concepts/Generated Ops；只安装 `public` headers 和实际库，不让插件 include `pinclude/sinclude` 或任何 `VulkanBackend` 私有 owner。

**动态 C ABI**：`extern "C"`、`abi_version`, `struct_size`, canonical type/schema identity, opaque DataSet/Record/Feature Handles, function table, explicit allocator/copy/retain policy, callback threading/error contract。跨 DLL 不穿过 STL owning containers、异常、RTTI hash 或不稳定 `typeid(T).hash_code()`；插件 Authoring C++ wrapper 在自己 TU 静态编译，Host type registration 只在 cold boundary 擦除一次。

## 11.2 内置类型优先复用

当已有 `LightGpu`/`MaterialGpu` 或公用 Image contract满足需要时，插件 `require<T>(ScopeKey)`，不复制一份 `PluginLightGpu` 或在内部创建重复 Buffer。若缺少合法数据类型，插件声明新的 DataSchema，并由 Host 同源生成器产生可安装类型信息；Host 复制和验证 Schema，再通过相同 DataSet/Graph/Layout/Natives 的 pipeline 使用。重复 type canonical 名但 layout 或 version 不兼容必须 fail，不能悄悄覆盖。

Type registration **不等于立即创建数据集**；一个插件可以仅新增一组值的 Schema，只有实际 Scope 调用 `create<T>` 才分配逻辑 DataSet。一个 DataSet 可以在 Feature 卸载后继续存在（仅当 Host 拥有其数据与安全 layout/CPU destructor/code contract），否则由 Scope 的生命周期事务撤销。

## 11.3 真正的插件 GPU 权限

Reader/Writers 与 GPU Views 是带 Access 和 Scope 的受限借用，不暴露 `VkDevice*`。写入 Provider 默认唯一 authority，多个 Consumer 可读。对于 GPU Pass，Authoring 通过 PassSchema 声明 `READ/WRITE/READ_WRITE` 和用途，Composition 校验其权限/Owner/Scope；不能通过自由录制 `vkCmdPipelineBarrier()` 绕过 Graph 的访问证明。高级 Vulkan tier 只在得到特定 Device Capability 和审查批准时开放受限原生 primitive，仍属于唯一 Native executor。

## 11.4 动态卸载顺序不可省略

```text
revoke new routes/data accesses → validate candidate composition
→ publish replacement graph/data bindings → quiesce CPU callbacks
→ wait/retire GPU in-flight old native backing/plan/descriptor
→ retire Feature state/callback code → release plugin code pin → unload DLL
```

DataSet 的 Host-owned POD/Schema metadata 可以在插件卸载后存在，前提是其 allocator/destructor/readers/writers 都不依赖已卸载 DLL。若删除记录需要插件自定义析构或提供转换函数，则不能先卸载插件。Static `FeatureOps` 函数表绝不等于 Code Pin；任何 callback 放在组件指针上保存都要跟踪来源插件和其最后使用。

## 11.5 外部插件门禁

使用真正的 installed SDK 外部进程/编译目录（无源码 private include）实现两个 Case：

1. 插件仅消费内置 `LightGpu`，新增自己的 Shader/Pass，输出像素正确，删插件后主 Shader 继续运作；
2. 插件新增自己的 `GaussianChunkGpu` 和 DataSet，多个 Feature 共享，真实 GPU Upload/Compute/Readback，卸载/重载验证 Schema 冲突、GPU in-flight、安全退休。

完成之前不能把 F1 元数据、F3 Binding 或 F4 Native 小例称作“第三方 Feature SDK 已实现”。
