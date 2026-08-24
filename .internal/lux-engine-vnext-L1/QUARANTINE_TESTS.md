# Quarantined test inventory

以下测试/演示依赖旧 ECS、旧 asset runtime owner、GPU builtin payload、旧 window host
或上层产品装配，不进入 vNext L1 的 `all`/CTest：

- `legacy/ecs/**/test`：除已在 reuse ledger 中重新表达的算法契约外全部延期到逐域迁移。
- `legacy/engine/**/test`：Player、Editor、Host、SceneRuntime、Toolchain 与实机装配测试。
- `legacy/modules/resource/asset-runtime/test`：AssetManager 生命周期、runtime load port、
  mutable shell 与 manager-driven typed SerDeser 测试。
- packed `render/features` 的 GPU shader/content tests：等待 Content/Toolchain provider。

允许保留并已重写到活跃图的测试只有 CPU/ABI、cooked wire/pak golden、同步 VFS、
L1 core/schedule/schema/snapshot/persistence 以及 hierarchy/transform vertical slice。
