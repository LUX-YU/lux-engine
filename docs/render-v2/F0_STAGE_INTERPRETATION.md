# F0 — 阶段解释与跨阶段证据归属

## 采用方式

用户已选择：原包逐字保存，另附经审阅的阶段解释表。本表落实获批准的实施计划，
不修改 FINAL 00–19 或四张 CSV，不删除验收要求，不将机制 fixture 计为完整业务能力。
阶段编号 F1 与能力编号 F01 必须区别；某能力跨阶段时全部必需证据齐备才关闭整行。

| 原包位置及差异 | 执行解释 | 完整证据边界 |
| --- | --- | --- |
| 03 §3.2、05 §5.2/5.8、19 §19.1 O-01 的 F1/F2 语法时点 | 按 19 在 F1 冻结现有 luxpass 的正式语法和 golden；F2 完成逻辑语义验证 | 不在 F1 宣称 C0–C8 或 native execution 已完成 |
| Capability I38、W18 与 15 的 Feature 阶段 | F1 验收 PassSchema/strong ID/生成语义 Concepts；F6 验收真实 optional Feature ops | 缺 optional hook 不生成空 thunk，须有真实 F6 SDK/ABI 测试 |
| W16=F4，I33/REQ05=F5 | F4 用原生 fixture 验证 layered output 与 separate fallback；F5 验证真实 Runtime/View 集成 | 不提前创建正式 View/Runtime；设备不支持的原生变体仍为 NOT_RUN |
| I36=F4，08/REQ22 的 View history | F4 证明 import/current/previous、条件和跨帧同步；F5 验证真实 View resize/cut/销毁 | 原生资源 fixture 不是 View history owner 的集成验收 |
| I32/F5 与 F12/W07/F8 的 Scene skinning | F5 验证 scope/input/epoch 去重与 View 独立输出；F8 以真实 Skinning 补齐 | 同名 pass 不作为共享证明，in-flight WAR 必须另验 |
| I12/I42/F3 与 F06/F8 材质热更新 | F3 验证 Shader/PSO 候选失败保留 last-good；F8 验证真实 GraphMaterial | F3 不提前实现 Material 业务或给整条材质链 PASS |
| H01 的 F11+H1 与 15/REQ32/W20 | F11 完成基础依赖和产品 parity；H1 实施并验收虚拟几何算法 | F11 不提前关闭 H01；普通 RenderCluster/indirect draw 不等价 |
| TypeOwnership 中粗粒度 Module/Owner/Phase | 按 02/04/08/09/12/19 的实际作用域、唯一 owner 和阶段权限落地 | 不因表中列在 Core 就提前修改 R1；不为凑表创建 131 个类型 |

Water 的上游输入在 F9 建立，F27 的完整 CRUD/Shader/resize 证据在 F10。
F7 的真实 Scene 发布、基础 mesh/light/camera 数据链不等于 F8/F9 完整业务 Feature parity。
F8/F9 的几何、材质、光照和阴影组合义务互相补证，不因较早阶段 fixture 通过而跳过真实组合。

## 已核对的源码落点与后续约束

- 当前 R3 的 `GraphPass::uses` 与静态 `CompiledGraphPlan::compile()` 只完成有限逻辑模型。
  F1/F2 按统一 owning Definition 迁移作者与消费者，不能保留旧 vector API 的永久影子实现或兼容别名。
- 旧 PassParams 模板位于 frozen Legacy，包含 Vulkan include、固定 8 字节 shared PC 前缀、128B 假设、
  旧 RGBuilder 与 descriptor 绑定。F1 迁移生成算法和输入事实到授权活动路径；不得配置/执行 Legacy 模板，
  不复制旧 owner hierarchy，不新增 parser。
- 当前 `modules/resource/description` 聚合 target 链接 Math/Script 等依赖。F1 仅整理中立 Shader 契约与
  生成桥，不让 Graph 通过聚合 target 引入这些依赖；Graph 本体维持 Core 依赖。
- R4 的阶段负例 `render.vulkan.reject_GRAPH` 验证的是 Foundation 隔离。未来 F3/F4 增加规范允许的 native
  Graph 层时，应保留 Foundation 的隔离证明，并对 native compiler 增加真实合法依赖证明。
  任何测试迁移都要有逐项映射、编译依赖证据和阶段审阅，不能删除失败用例换绿；F0 不改任何测试。
- R4 的 Device/VMA/Buffer/Image/Descriptor/Pipeline/Submission/Staging/Retirement owners 继续复用。
  F3/F4 的明确扩展不授权推倒 Foundation 或新建第二设备体系。

## 不改变的门禁

C++20、AGENTS.md 排版、expected/结构化错误、完整 RAII、三 Lane 因果关系、reply consume-or-abandon、
WRITING 写者回收、single Runtime/Backend/FrameLoop、独立 Render Progress、Backend 计划共享、GPU fence
退休、插件 code pin、稳定路径零首方 C++ heap、可比 p50/p95 未解释超过 5% 回退 STOP，全部继续适用。
先进算法和五种 PointCloud/PCF+EVSM+CSM 等既有变体不能被 fixture、命名或较低画质 fallback 替代。

## 后续报告如何登记

原包四张 CSV 保持原始要求与哈希。每阶段报告按原 capability/REQ/Workload ID 登记当前证据、实现 SHA、
源文件、owner、测试及归档 hash；未完成的关联项继续列出。报告的 qualification SHA 由承载报告的 Git
提交解析，不预填自引用 SHA。汇总只能从已提交的独立报告推导，不新增另一套手写可变状态账本。
历史 I/V 报告不回写，F0 的文档 PASS 不代表任何新 Graph、SDK、产品或先进算法 PASS。
