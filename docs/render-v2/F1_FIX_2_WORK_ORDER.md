# F1-FIX-2 — 资源局部身份与访问合法性

## 授权与锁定范围

用户授权基线：`bf922e84235487dcdddf0c8a8c1c75ac29abadf8`，分支 `codex/render-v2`。
开工时实际远端与本地 HEAD 一致，V2 worktree 干净。证据保存于源码树外。

本轮只修改 `modules/function/render/graph/**` 和新增 `docs/render-v2/F1_FIX_2_*`。
不修改 Meta generator、Shader emitter、Description、bootstrap、Core、Transport、Vulkan、
Frozen Legacy、FINAL 正文或既有验收报告。不实现 F2 Hazard Scheduler、Native Graph 或 Runtime。

## 合同与责任

- `GraphResourceHandle<Tag>` 是值引用：现有 StrongId 保存声明位置，加一个不拥有对象的作者作用域值。
  `GraphTexture` / `GraphBuffer` 类型隔离。数值 `value()` 仍是位置，不能单独证明归属；
  原始数字构造只有未绑定的位置，`addPass()` 拒绝。`isValid()` 只说明非零位置，不代替归属检查。
- 每个 Builder 获得单调 64 位冷态作用域。实现只有一个原子计数器，不保存任何对象表，
  不分配逐句柄状态，不使用 Builder 地址，不依赖 Builder 的存活；计数耗尽 fatal，不循环复用。
  该值属于当前 Graph 模块的作者域，不是序列化身份、插件 ABI 或 Runtime handle。
- Builder 不可复制。移动转移已有声明及其作用域；移出方立即成为有新作用域的空 Builder。
  `finish() &&` 同样消耗原作用域，成功或失败后均可重新构建；旧 token 不命中新槽位。
- Generated PassSchema 仍是唯一字段来源。捕获保存每项实际选择（包括 optional fallback）的
  作用域，`addPass()` 在丢弃作者作用域前核对归属、位置和 Texture/Buffer kind。
  `CapturedParameters::authoring_scopes` 仅为此次冷态捕获存储，不进入 Definition 或计划比较。
- `GraphFieldBinding::resource_kind` 保存 typed wrapper 的真实种类；Definition 再核对实际资源和字段角色。
  sampled/storage image、attachment/resolve/input attachment 必须是 image；UBO、SSBO、vertex/index/indirect
  必须是 buffer；transfer 可以是任一种，但必须与所捕获 wrapper 一致。
  原 R3 内部 whole-resource oracle 继续经相同 validator，不新增公开裸 Definition 入口。
- Depth/Stencil 格式的有效 aspect 仍唯一来自 `rdesc::textureAspectMask`。
  非零 aspect 必须为格式所支持的 depth/stencil 子集；对未声明 aspect 的 load/clear/store 一律拒绝。
  stencil-only view 必须将 depth load/store 显式设为 DISCARD，反之亦然；clear 数值本身不是一次访问。
  DISCARD 不否定已声明 attachment 写访问，LOAD 仍产生 READ_WRITE。完整跨 Pass hazard 留给 F2。

## 类型与性能边界

新增强类型模板只负责作者引用的 kind 与局部作用域，不拥有资源、不实现注册表。
Builder 唯一拥有可变声明；Definition 唯一拥有经过验证的快照。
作者 token 和 Params 的 CPU 布局改变后，生产 Meta job 必须重新生成 sizeof/offset/Shader 元数据并对账。
稳定 FrameGraphBindings、Plan 与 Binding benchmark 源码不变；不引入逐帧 scope 校验或分配。
冷态 capture 的临时 scope 向量允许分配，必须如实记录冷态成本，不能宣称它零分配。

## 新增验收与旧义务

原 92 个 CTest 名称、测试实现和语义保留，新增七组：

1. `fix2_same_kind`：不同 Builder 同 Kind 同数值槽位，以及 optional fallback 与 Buffer 路径。
2. `fix2_cross_kind`：不同 Builder 不同 Kind 的同槽位，Texture 和 Buffer 两个方向。
3. `fix2_lifecycle`：移动、移动赋值、finish/reuse、相同地址析构重建；不同作用域不污染 Definition 等价性。
4. `fix2_forged`：原始数字构造与保留作用域的故意 bit_cast 错误 Kind，两方向均拒绝。
5. `fix2_roles`：所有 image/buffer 角色、transfer kind、伪造内部 binding 的独立 Definition 校验；真实生成的合法 SSBO/image。
6. `fix2_depth_ops`：depth-only 非法 stencil 操作、DS 缺失 stencil aspect、合法 depth-only/stencil-only/combined。
7. `fix2_aspects`：零/未知/color/depth/stencil 与格式的非法组合。

普通和完整 ASan 构建、第二轮无工作、全 CTest、Meta → emitter → glslc → SPIR-V reflection
合同、真实 File API/include/link/codegen 闭包、三安装前缀同步、719 Legacy、历史报告与用户六处修改保护，
均在实现提交 I 后由独立 clean clone 验证。完整 ASan 继续使用独立构建且 annotations 匹配的 SPIRV-Cross，
不屏蔽 STL annotations。稳定 Binding 使用上一批准版本与新 I 的七组交替百万次样本，
p50/p95 无法解释的 >5% 回退停止验收；记录 allocation count/bytes 和全部样本。

F3 前置缺口单独列于 `F1_FIX_2_F3_PREREQUISITES.md`，其状态不因本轮通过而关闭。
实现 I → 独立资格 → 仅新增验收报告的 V → 推送 → STOP；F2 仍需用户复审授权。
