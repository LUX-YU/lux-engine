# RD3-P2 — RenderEffect/Feature 与中立 GraphIR/Cache（DRAFT，NOT_AUTHORIZED）

## 目标

以 `RenderEffectId` 为可编辑共享效果权威，以 Backend-owned `RenderGraphId` 为 immutable code identity。一个 Effect 可以有多个 Target-specific Native Graph；不同 Effect 可以在 full structural equality 成立时复用同一 code。Scene/View 不再拥有 Feature/Graph，对后端的 Target/Scene 更新无需经过 Feature 注册。

## 必做

1. `RenderEffect` 实际维护 FeatureType+配置和 revision、candidate/active、可用的 build CodePin/ops。Feature 作者用 C++20 concrete build+普通公用算法，静态无状态优先；动态注册边界固定 Ops 表，只在 cold composition 执行。
2. `RenderGraphBuilder → Definition → RenderGraphCompiler → RenderGraphIR` 完成单 Target 最终输出和所有内部 GBuffer/HDR/Shadow Scratch；复用当前 C0–C8 的版本/依赖/Scope proof、完整缓存身份和局部资源 ID。
3. Backend 维护 Graph Cache 与 `RenderGraphId` 全相容键、生成式 lifetime，View 只有 EffectId choice，GraphId 派生索引冷态准备；逻辑 Effect 结构更新候选验证/rollback 的 CPU fixture。
4. 删除 Legacy 风格 Native Graph 长期捕获 Feature `this` 的路径，不允许每个 Feature 各持 Native PSO/Shader 的最终所有权。
5. 扩展真实 shader/source shape 时只使用 F6.a/b 已有 DataSchema/PassSchema 生成链，保障多 Owner SPD/PSO 共享与 full equality。

## 接口例与反例

```cpp
// Target-only Authoring：feature.build(graph,config) 后无运行期 callback 引用。
auto effect = backend.createEffect(effect_desc);
view.setEffect(effect.id());
auto graph = backend.compiledGraphFor(view,target);
// 反例：Scene.addFeature() 改写 SceneGraphCache，或 Graph 与 Feature 保存双向裸指针。
```

## 验收

两 Scene/四 View 一 Graph IR code/一个 Effect，E1/E2 结构去重、Effect 热参数不变 GraphId、Topology 编辑产生新 GraphId/全部候选 rollback、Plugin generated ops 正负例、Resource ready fixture、实际 GPU 最小消费者、Full ASan/动态 ABI/warm zero heap。比较受影响 native code/perf，不为重构新增多层 Native CompilerPassManager。I→clean V→STOP；不得自动 P3。
