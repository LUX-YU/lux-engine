# Lifecycle LR02：Vulkan/VMA 叶子所有权资格

LR02 Windows 阶段门禁 **PASS**。固定实现为
`fbc252940e79c474fff1d1511e8af8472c3c49c7`，lux-cxx 为
`0a0e7419fc7229df6e372cd35a540249f92250ef`。本结论仅覆盖叶子资源能力及本阶段实际消费者；
聚合对象的完整构造、异步物理退休仍分别属于 LR03、LR06。

## 实现与唯一责任

| 实现提交 | 实际闭包 |
|---|---|
| `e9f7fddb0` | Arena 完整创建；失败不发布虚拟段，拒绝非法对齐 |
| `7bd1a7872` | DescriptorService、布局、池及原代际退休中的 native owner |
| `ccc751278` | ShaderObject module owner；移动替换释放旧模块 |
| `6b0fb180c` | GeneralDescriptorSetLayout 两组布局 owner；禁止失真的按值移动 |
| `a0a4a8117` | Transfer timeline 和固定 command pool owner |
| `2c03dc033` | DeviceContext 的 VmaAllocatorOwner，保持 allocator-before-device 顺序 |
| `b8dc17981` | Offscreen view owner，沿原 resize 候选及 watermark 退休 |
| `1e750900f` | RGRecordContext 自建 view/query/pool/semaphore owner；import 仍为借用 |
| `160176ac6` | 一次性 command buffer/fence，失败提交与等待按原设备安全点清理 |
| `f5b55c3a4` | RGVulkanResourceAllocator custom pool owner，缓存 allocation 先于 pool 释放 |
| `0e611492d` | PipelineManager 实际 cache 拥有 render pass/compute/graphics pipeline |
| `fbc252940` | 已正确 VmaBuffer/VmaImage/StagingBuffer 的实际资格；显式 native GPU 测试模式 |

所有新 native owner 保留精确设备或 allocator 来源。Vulkan callback 指针是准确借用，
不伪造 callback 上下文所有权。移动替换先释放目的资源；原 cache、generation、refcount、
worker、fence/watermark 和设备关闭算法保持单一实现。Pipeline cache 先释放 pipeline 再释放 pass；
immutable sampler 仍晚于引用它的 layout 销毁。删除零消费者 `PipelineManager::destroyAll()`。

冻结 LR00 表中 65 条叶子及组合责任均映射到具体 owner/消费者。组合标为 LR02/LR03 的行
仅完成本轮基础能力，未把后续调用迁移计为完成。计算管线的布局/反射提前发布、Bindless
忽略 transition 失败、聚合 init/shutdown 等仍明确保留为 LR03 工作。

## 修前输出与边界

四个真实修前失败保留：Arena native 创建失败仍发布容量；Shader 移动替换泄漏旧 module；
General layout 移动遗漏 domain layouts/capacity；Bindless 原生 begin 失败泄漏 command buffer。
测试不只判断返回错误，还核验存量 cache、准确释放、输入资源及重试。

初次构建缺少 VMA include/implementation、两个 timestamp raw borrow 漏迁、注入函数名称冲突、
旧 SDK layout 实现与新私有头混用等失败均保留，分类见归档
`scripts/failure-classification.json`。没有通过扩大生产导出或删除断言使夹具通过。

Pipeline fixture 使用真实 PipelineManager 算法与确定性 native endpoints，不能冒充真实
native pipeline 创建资格；实际渲染由另行执行的框架 GPU 测试支撑。callback 来源测试不代表
自定义 Vulkan allocator 的完整驱动资格；此桌面不具备 tiler lazy-memory 硬件资格。

## 固定提交验证

先执行 `ValidateTrackedSnapshot`，独立干净检出固定实现，再刷新现有独立构建树；
**不宣称冷构建**。全新 SDK 前缀此前不存在，消费者不回退开发 DLL。

| 验证 | 实际结果 |
|---|---|
| Editor all / 第二轮 | PASS / no work |
| Editor CTest | 88/88，显式 `LUX_RENDER_BUILD_GPU_TESTS=ON` |
| PLAYER all / 第二轮 / CTest | PASS / no work / 48/48，native GPU 模式 OFF |
| 原安装框架消费者 | 16/16 |
| 独立 Window / Physics2D / Script 消费者 | 1/1、1/1、2/2 |
| 真实窗口模式 | `--desktop` 与 placement 通过；不是系统输入接管 |
| 12 组 scoped MSVC ASan | all / no work / 12 个程序通过 |
| 实际依赖闭包 | Editor 621、PLAYER 565、框架 SDK 59 个 TU；最小消费者 1/1/2 |
| 叶子审计 | 65 条冻结责任逐项映射，旧替代体删除，公共头未修改 |

ASan 对实际叶子及消费者实现插桩，未宣称整个 Renderer 或所有依赖 DLL 插桩。
GPU 模式关闭时仍编译第一方生产实现及测试目标，仅不注册需要真实 device 的运行测试。
本轮修改的头位于 sinclude/pinclude，不涉及安装公共头，因此没有新增三前缀同步成绩。

## 归档、未测与后续

外部证据：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr02/verified-evidence/`。
共 1,211 文件、160 条命令记录；manifest SHA256：
`040890afca84237fcac84ba37e96ebb6ba8375ec8d935aeb101d95da304d85fa`。
相对路径验证、中文/空格搬迁通过；删除和篡改真实 Script SDK 输出均被拒绝。
开发 dirty diff、修前版本与最终 clean SHA 分别记录，不以历史输出冒充本轮执行。

源码/SDK 原生输入仍为 `NOT_RUN_USER_DEFERRED`；Linux、IME、旧性能范围不改判。
UBSan `NOT_QUALIFIED` 与 `Q-P06-CLANG-GET_DELETER` 保留，未新增 Clang 全量成绩；
后续总资格仍须处理明确义务。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。
Context/Pane 用户排版未提交，ProjectBuilder 补丁未应用；main、原验收记录不变。
按连续实施授权进入 LR03，先处理聚合候选的真实失败发布，再移除构造状态协议。
