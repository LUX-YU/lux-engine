# RD2-D0-C1 一致性审阅与目标 API 责任

本页是 Implementation 的设计审阅材料，**不是独立 Verification**。基线 b9f9a909bc34；全部新 API 均为目标设计。本轮未编译示例或执行 CPU/GPU 测试。具体源码锚点及 Git blob 见 RD2_PROVENANCE。

## 1. 唯一 authority 与依赖

| 事项 | 唯一责任 | 反例及拒绝理由 | 对应正文 |
|---|---|---|---|
| 类型 | Core 稳定语义 ID；同源 DataSchema 扩展字段/可选表示 | 同尺寸不同类型不可互换；版本不拼入既有哈希算法 | 02 §2.3、03 |
| DataSet | Runtime/Scene/View 的实际 Scope 决定逻辑寿命和 writer 权限 | Feature detach 不可销毁仍有 Scope/consumer 的集合 | 02 §2.2–2.5、06 |
| Native backing | Backend 内实际 RAII owner 独占 Vk/VMA 销毁责任 | DataSet/Feature/CPU ResourceDomain 不能各自释放同一分配 | 02、04、09 |
| 资产 | RenderResourceDomain 保持 CPU semantic retain ledger | 两 Scene 共享资产不等于共享 Light/Visibility；不复制上传或夺取 VMA owner | 02 §2.4、10、12 |
| 需求解析 | 冷态 Feature Composition 解析 Type/Scope/Key/Capability/Writer | 无唯一匹配或 fallback 时冷态失败，不按 attach 次序选择；不进入 Graph Registry | 06、07 |
| Graph | 原 C0–C8 基于真实 typed resource access/producer/version | CPU-only needs 无 GPU use，不生成伪 Pass/Barrier；GPU writer 才形成真实版本边 | 07 |
| Native | 原 C9–C13；C11 配方、C12 同步 | CPU pass 序号不证明跨 Queue 或跨帧不重叠；不改 F2 来掩盖 native 风险 | 07、09 |
| Shader | 同一 meta generator、PassSchema、LayoutPlan | 无第二 parser/反射 registry；禁止手工 set/binding 或反射子集冒充 full shape | 03、08 |

Composition 先冻结逻辑 StorageInterfaceShape；Graph 据此验证 byte/mip/layer ranges；C10 根据实际 usage 和 Caps 选择相容 backing。若 SoA/BDA/Atlas 会改变 Shader ABI，必须在 shape 冻结前选择已定义变体，不能在 C10 偷换。此数据流无 Graph↔Storage 分配循环，也不增加常驻 Compiler/StorageManager。

## 2. 寿命与缓存推演

| 场景 | 应保持的事实与预期结果 | 后续证据 |
|---|---|---|
| 两个同 T DataSet 的 slot/gen 相同 | handle 必须携带或绑定 DataSet/Scope；外来 handle 拒绝；本地合法 handle 接受 | F6/F7：owner/generation 正负例 |
| CPU patch 已完成、UPLOAD 未 Ready | observed/applied 不等于 GPU snapshot；保留上一合法快照或明确暂停该目标；未 ready 不读 | F7：背压、跨 Lane 反序 |
| erase 后旧 GPU 仍读该 slot | CPU 身份失效；字节/active list 复用须 GPU completion 或合法新 backing | F7：3+ FIF reuse/resize |
| Feature A/B 共享一集合，删除 A | 撤销 A 权限/Pass/code；B 的集合继续有效；Scope 删除才撤销集合 | F6/F7：detach/readback |
| Plugin 提供 CPU-only 非平凡 T | Host 复制 Schema 不能取代插件析构/转换 code pin；依赖仍在则禁止卸载 | F6：installed DLL 卸载负例 |
| Scene A/B 同结构、不同 DataSetId | BindingSlotKey/full shape 相容则可共享 Shader/Native immutable plan；Invocation 绑定各自 backing | F6：DF06；F5 只验既有接口可表达的相容计划 |
| 同 Scene main/preview | 同类型不意味着同实例；语义槽不同，未给唯一 key 时拒绝 ambiguity | F6/F7：DF04 |
| Camera/revision/native handle 改变 | 有真实 Invocation 动态输入；不得导致相容 PipelineLayout 重编译；GPU ABI/Target shape 变化另判兼容 | F5/F6 |
| 四 View 共用计划 | 四份可变绑定/history；Scene 工作只有严格输入/epoch/副作用证明相同才合并 | F5；真实 Skinning 留 F8 |
| Device Lost | 当前 Runtime 终态失败，不伪造完成；宿主未来可另建 Runtime | F5；D0 不实现恢复 |

## 3. 新目标类型责任清单

所有名字是设计职责，不要求逐个制造包装类。已足够由值/自由算法表达时不新增 owner。

| 目标类型/机制 | 类别、实际状态 | 创建/销毁与线程边界 | 失败/必要性 | 阶段 |
|---|---|---|---|---|
| DataSchema<T> | 生成 Traits/value；语义字段、Scope、authority、版本 | 编译生成；Host 冷态复制注册描述，不借插件临时 span | collision/version/字段不合法；避免重复手写事实 | F6 |
| GpuLayout<T>/GpuRepresentation<T> | 可选生成值/转换；GPU shape、offset、stride、fingerprint | 同一 generator；离线/冷态验证，不每帧解析 | Shader ABI/Capability 不符；CPU struct 可不同 | F6 |
| DataSetId / RecordHandle<T> | Scope-local 强身份值；owner/key/generation 与记录身份 | Scope 创建/撤销；跨线程编码合法句柄，不传裸指针 | foreign owner、stale generation、wrap 不复活 | F6/F7 |
| DataSet | 真实 Scope 逻辑 authority；记录、writer、revision、retention | Backend owner-thread；Scope 事务销毁，GPU backing 独立退休 | 缺 scope/key、冲突 writer、容量；不能成为第二 Vk owner | F6 最小/F7 完整 |
| TypedStorage<T> | CPU record owner + 薄模板算法；slot/dense/dirty | 合法 owner-thread；按 T 生命周期构造销毁，GPU-only 无伪 CPU mirror | stale/capacity；非 T 的 slot/dirty 算法集中复用 | F7 |
| DataSetReader/Writer | 短期受限 borrow；已绑定权限和集合 | 不跨线程、不异步逃逸；Writer 同步 patch 捕获 dirty | unauthorized/stale/foreign；不给长寿裸 T& | F6/F7 |
| GpuDataView/DataSlot | 结构性逻辑 view 或 invocation borrow，两种时点明确 | 冷态降低为 GraphBuffer/Texture；运行期绑定实际 native receipt | type/range/ready 错误；不持第二份 native ownership | F6 |
| StorageInterfaceShape | 中立值；GPU ABI/资源形状/寻址约束 | Composition 冻结后作为 Graph 作者输入 | 非法形状；不含 Vk 分配和 DataSet registry | F6/F7 |
| StoragePlan | 冷态计划值；CPU 策略与相容 native recipe | 无状态算法分两步产生，变更走候选事务 | 不兼容 ABI/usage/budget；不是常驻 Manager | F7 |
| BindingSlotKey | 结构性强身份；schema/semantic/scope/full shape | 冷态 Layout/PSO key；exact comparison；实例另绑定 | collision/shape 冲突；不等于 set number/DataSetId | F6 |
| NativeStorageBacking | 必要 move-only RAII；已有 Buffer/Image 等实际 native owner | Backend；Ready/last-use 引用到 completion 后退休 | device/capacity/creation；复用 R4 不建第二 VMA | F6 最小/F7 完整 |
| Typed Workset | 具体 GPU 表示的索引/可见列表值或 Graph scratch | GPU Pass 按实际版本读写；需要跨帧才有 Scope history | 无通用 GeometryKind/TypeId Shader 分派 | F7 机制/F8–H6 业务 |

旧 131 行逐项责任映射见 RD2_TRACEABILITY；表内历史候选名字不表示当前均存在。

## 4. CPU / Wire / GPU 文档测试规范

以下为目标示例与预期接受/拒绝，**全部 NOT_RUN（D0 无生产编译）**。

| 编号 | 正例 | 必须拒绝的反例 | 预期验证阶段 |
|---|---|---|---|
| DATA-CPU | CpuLabel { std::string text; } 具有 DataSchema；CPU storage 采用正确构造/移动/析构，合法 emplace/patch | 未声明转换便把 string 对象字节送进 SSBO 或 memcpy 为 Wire；插件析构代码已卸载 | F6/F7 CPU/SDK |
| DATA-WIRE | LabelUpdate 显式编码版本、目标句柄、长度和 owned UTF-8 内容，复用既有 Transport packet/attachment | 异步包借临时 string/span、错误版本或越界长度；没有 GPU Layout 却用于 typed GPU view | F6/F7 codec/Transport |
| DATA-GPU | Light CPU 表示通过同源 GpuRepresentation 转为两组 float[4]；GPU offset/stride/align 与 reflection/数值对账 | 同 size 不同字段布局、错误 stride、绕过生成器手写第二 GLSL struct；GPU-authority 调 CPU patch | F6/F7 Shader/GPU |

## 5. 作者成本：显式设计样本而非性能成绩

统计边界：SDK 基础已由相应阶段建立；每个样本列出的作者文件计一次，生成物不计手写文件；生产代码修改数是目标约束，不是本轮实测。宿主统一插件装载入口不按每种业务类型添加 switch。

| 样本 | 手写文件集合（设计包，不创建这些生产文件） | 新文件 | 核心业务特判修改 | 作者公共声明类型 | 重复字段/绑定声明 | 拒绝点 |
|---|---|---:|---:|---:|---:|---|
| LightGpu 数据无 Feature | LightData.hpp、LightPublish.cpp | 2 | 0 | 1：LightGpu | 0：字段只声明一次 | Schema collision、GPU stride、foreign DataSet |
| Sprite2D 无 Mesh | SpriteData.hpp、SpriteParams.hpp、SpriteFeature.cpp、SpriteCull.lglsl、SpriteDraw.lglsl | 5 | 0 | 3：SpriteGpu、CullParams、DrawParams | 0：各 Pass 访问不是重复数据定义 | 缺 INDIRECT、未初始化 args、CPU-only 当 GPU |
| 两 Owner 单 Shader | DualOwnerParams.hpp、DualOwnerFeature.cpp、DualOwner.lglsl；复用既有 Light/Material | 3 | 0 | 1：DualOwnerParams | 0：full shape 复用生成信息 | 同角色多 owner 误合并、main/preview ambiguity |
| 外部 GaussianChunk | GaussianData.hpp、GaussianParams.hpp、GaussianPlugin.cpp、Gaussian.lglsl、CMakeLists.txt | 5 | 0 | 2：ChunkGpu、PassParams | 0：现有 C ABI/ops 生成 | ABI/私有头/code pin、非法布局、卸载仍借用 |

文件集合是成本计数模型，不冻结最终文件名/实现文件划分，不证明 Shader 已运行。两 Owner 案例数据类型数量为 0 新增；Feature 具体类若需要对外暴露，实施阶段必须如实上调公共类型数，不用隐藏文件降低统计。

## 6. 结论边界

已按用户裁决消除候选文档中发现的身份/编译阶段/Composition/CPU-Wire-GPU 冲突。其语义成立仍需 F6/F7 真实实现证明；API grammar、GPU packing、storage 阈值、generation 位宽和 concrete slot 布局不在 D0 伪造为已冻结生产 API。

F4 静态风险尚未实机复现：direct compute→host readback、不同 handles 的实际 memory overlap、import A→B 旧 view/backing。它们与 fixed binding 性能及原 F4 缺失能力归 F4-C 另行授权。历史 146/146、ASan、GPU 0 error 只引用 F4_VERIFICATION，不作为 D0 新测试。
