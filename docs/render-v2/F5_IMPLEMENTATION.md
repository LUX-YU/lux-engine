# F5 实施记录（未取得阶段资格）

入场与权限见 `F5_ENTRY_DECISION.md`、`F5_NATIVE_IMPACT.md`。本文件记录开发事实，不替代独立 Verification。

## Native Plan / Invocation 接缝

| 类型 | 唯一职责、所有权 | 线程与失败 | 更简单表达的边界 |
|---|---|---|---|
| ExecutableGraphPlan | move-only 对外持有不可变 NativeGraphCode；Shader、Pipeline、Layout、结构 recipe 的唯一共享存储 | 冷态工厂 expected；发布后只读 | 不能再用同一 owner 的 frame slots 表示任意 View/Invocation |
| NativeGraphCode（私有） | 冷构造的 LogicalPlan、NativeShaderProgram、资源形状、Sync2/alias recipe；发布为 shared_ptr<const ...> | 编译期可变，发布后不可变 | shared pin 只在执行 owner 冷态创建时取得；逐次 submit 不复制或增减 refcount |
| NativeGraphInvocations | 有界 GPU backing、ImageView、Descriptor、scratch、queue ticket 的 move-only owner；显式 invocation slot | 单 Backend 执行域；容量/错误通过 expected；析构完成最后借用 | 容量与 FIF 分开；仍使用已有 R4 native owners，不重复拥有 Vulkan handle |
| NativeCompileInputs | LogicalPlan、设备、Queue 位置、Native command 和 alias 选择的短期冷编译借用 | 仅工厂调用期间 | 已移除 allocator、初始 Import backing、动态值与 frame capacity |
| NativeInvocationInputs | 冷态创建执行存储所需的 allocator、初始 binding、动态值、sampler 和独立容量 | 借用依赖必须覆盖实际 native 使用期 | 不承担 Shader/Pipeline 编译，不是第二 Graph |

跨 Invocation 的 FrameGraphBindings 仍仅为同步 borrow。最终 Runtime 必须拥有各次提交需要的输入快照和 native lifetime；不能持有调用栈临时 span，也不能用 CPU revision 代替 GPU completion。

旧测试的 `compileFixture` 仅把两个真实 production factories 顺序装配为测试值，测试显式访问 `.plan` 或 `.invocations`；没有 production 兼容入口、第二调度器或 fake Shader。原 benchmark 完整冷工作量继续包含所有 backing 和 descriptor 创建，其阶段计时按两个工厂累加，不能只测纯 Plan 工厂冒充等价结果。

旧 `frame_capacity=1` 的拒绝断言不能套在新的 Invocation capacity 上：一个 Invocation slot 是合法的独立容量，零容量非法；Runtime 的 FIF 约束须在实际 Runtime 配置中单独验证，不能遗失旧两/三 frame 回归义务。

## 已运行的开发回归

证据：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-F5/development/`。

- Native Graph target 与迁移后的测试目标构建成功。
- `const-regression.log`：13/13；原 native_graph、graph_candidate、C1/C2/C3、五种 Draw，加新的 native_invocations。
- 新用例：一份包含 compute/graphics Pipeline 的 Native Plan，四个 View，2/3 FIF 对应 8/12 独立 Invocation slots；逐 View exposure 不同，所有像素与独立 CPU Tonemap 公式一致。
- 一批提交仅等待最后 completion，然后读取全部早期回执；没有 waitIdle 或每 View 新建 Plan。
- 24 个预热后批次提交路径首方 C++ allocation count/bytes 均为 0；Validation 0 error。Camera/history/target identity 在 fixture 中独立，但不据此宣称 Runtime/Target/History 功能已经完成。
- 已同步受改公共头至三安装 include 前缀并保存原字节。本项不是 installed SDK 或 Android 资格。

尚未执行本阶段独立 clean-clone 资格、完整 Normal/ASan、最终性能、Runtime/Presentation 与生命周期验收。F5 不能据此标 PASS。

## 未完成门禁

Runtime 执行线程与 Transport 装配、Scene 工作共享的原生证明、共享 Asset backing、真实 Target-relative extent/Resize/Minimize/Presentation、Stop/Device Lost、独立 pacing、诊断出口、完整类型/依赖清单、最终 I/V 和性能资格仍须完成。
F4 冷态 p95 的延期结论保持原样；F6/F7 尚未授权。
