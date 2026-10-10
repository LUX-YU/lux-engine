# LuxEngine Render V2 — F3 合并正式实施工作单

**合同编号：F3 / 2026-10-10 · 状态：AUTHORIZED（仅 F3） · 唯一正式实施阶段，内部步骤 F3.0–F3.4。**

> **交付给实施 LLM 的指令。** 本文替代此前独立的 `F3-PRE` 工作单与「F3 待授权草案」的执行安排，将两个已知上游缺口作为 F3.0 的**内部强制门禁**，而不是另设需用户审批的阶段。实施方在同一份 F3 授权内依次完成 F3.0→F3.4，最终只进行一次**最终实现提交 I + 锁定 I 后独立资格验收 + 仅新增报告的验收提交 V**，推送后 STOP。中间可有清晰的施工提交/验证记录，但不得把仅完成 F3.0 的结果报告为 F3 PASS；如果 F3.0 未通过，不能进入其依赖的 F3.1–F3.4，更不能以补偿性 Native 代码掩盖缺陷。
>
> 禁止以简化 MVP、跳过 GPU、虚构或静默降低 Shader 工作量、删减旧测试、添加第二 Native Owner、第二 Shader Parser、延后定义 Descriptor 语义等方式取得表面 PASS。凡必须改变已冻结的架构决定或越出授权文件范围的情况，提供最小复现、依赖链、方案比较并 STOP 等待用户批准。

## 0. 版本身份、授权边界与入场动作

| 项目 | 必须核验的真实身份 |
|---|---|
| Repository | `https://github.com/LUX-YU/lux-engine` |
| Branch | `codex/render-v2` |
| **已批准的 F3 入场 HEAD / F2-FIX Verification V** | **`c986e69765c9dd222469d4cf5090aa72549966dc`** |
| F2-FIX Implementation I | `946cf0866eda3566abc2fe2d06581ff868e157c3` |
| Frozen Legacy reference | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| F2-FIX 最近验收 | 普通 CTest **124/124**、完整 MSVC ASan **124/124**；旧 116 项保留；C7 Scope 修复 PASS |
| F3 完成前产品状态 | `V2_PRODUCT = EXPECTED_UNAVAILABLE` |
| F3 的后续阶段 | **F4 不授权；完成 F3 后独立审核并 STOP** |

**开工前的硬动作：**

1. `git ls-remote` 核实远端分支 HEAD；检出施工专用 worktree，保存 `HEAD/branch/status/staged/unstaged/untracked` 和用户原工作树**六处原始修改文件**的 SHA-256 与原始二进制 diff。不得 `reset`、`stash`、覆盖、以 git checkout 清除用户修改。
2. 如果远端 HEAD 不等于本表、存在未审查新提交、保护对象不一致，则只提交差异报告并 STOP；不得将当前指令自动迁移到新基线。
3. 实际阅读 `docs/render-v2/final/00_README_权威总纲.md`、`02_分层_类型_所有权.md`、`04_RenderGraph_作者模型.md`、`05_Shader_自动绑定.md`、`06_逻辑图编译_优化.md`、`07_Vulkan编译_执行_同步.md`、`12_Cpp20_实现哲学.md`、`13_错误_性能_生命周期.md`、`15_实施阶段_工作范围.md`、`16_验收与功能矩阵.md`、`17_LLM实施合同.md`、`19_冻结决策与歧义消解.md`、`docs/render-v2/F1_FIX_2_F3_PREREQUISITES.md`，并对照已有 F1/F2/F2-FIX 源码和实测证据。部分章节示例只表达**目标语义**，不代表现有源代码已实现。
4. 盘点当前 CMake target、public/private includes、编译/链接/生成/安装闭包及实际消费路径。将计划修改文件逐个标为 `ALLOWED`、`CONDITIONAL` 或 `FORBIDDEN`，连同理由、目标功能和测试写入 `docs/render-v2/F3_IMPLEMENTATION_INVENTORY.md`。
5. 记录 R4 原有真实设备/资源 Owner 与工具链版本；冻结 719 Legacy 文件、R0–R4/F0–F2-FIX 验收报告的 Git blob/mode/size 身份；不把 F2-FIX 的既有 PASS 重新解释为 Native PASS。

### 唯一架构裁决依据

以 FINAL 第 05、07、12、13、15、17、19 章和 D13–D17、D21、D24、D27–D31 为权威。F3 的实际交付是：**真实可用、在冷态完整编译的 Shader/LayoutPlan/Descriptor/Pipeline 资产、严格的 ABI 与 Native Layout 证明以及实际 GPU 数值验收**。F3 不是 F4 的全图录制/Queue/Transient Graph 编译器，也不是 F5 的 Runtime。

## 1. 单阶段目标、数据流和完成边界

### 2026-10-10 用户偏离审查修订（本 F3 合同内生效）

本修订不新增阶段、不改变 FINAL 冻结文件或历史资格事实。每阶段入场先核对 FINAL
要求的能力、工作负载和文件权限，再安排具体实现与测试。内部缺陷在当前授权职责内修复和
重新验收；只有冻结架构冲突、额外文件权限或可复现阻断才请求新的设计决策。

| FINAL 编号 | F3 必须交付与独立证据 | 不得提前关闭的责任 |
|---|---|---|
| W01 | 同源生成 Tonemap、Blur、Composite Params；三者实际 GPU 输出、Blur/Composite 多输入自动绑定、Shader 变体及 CPU 图像参考 | F4 通用 Graph Recorder；F9 完整后处理 Feature |
| W15 / I11 | 多个 Pipeline 使用同一完整 Owner Shape，互补 VS/FS 子集的真实 GPU 数值证明 | F5 Scene/View 装配；F8 实际 Material/Light 业务 owner |
| I09 | 唯一 LayoutPlan、所有声明 descriptor 类型/数组/PC/预算与 native layouts | Native Graph 资源和同步规划 |
| I10 | 原始及最终 SPIR-V 身份、成对重定位、spirv-val、重反射和错误输入拒绝 | 每帧编译或重定位服务 |
| I12 | Shader Asset 稳定身份、已编译 Variant 的完整身份和有界冷态目录、替换/删除/借用生命周期 | Runtime 文件监听、自动热更新线程或新的资源 Manager |
| I42（F3 子项） | Shader/Variant 缓存完整比较；Native Candidate 失败保留 last-good；成功替换后以 R4 完成证据保护 in-flight 旧 PSO | **I42 整行保持 PARTIAL，F8 补齐真实注册 Material 和材质热更新证据后才可关闭** |

资产、变体与 Native Candidate 是三个不同责任：Shader Asset 表达来源稳定身份；Compiled
Variant 拥有 source/dependency/defines/toolchain/schema/stage 与实际 SPIR-V 的完整编译身份；
冷态目录在明确容量内拥有变体，查找不能仅凭 hash 判等；Native Candidate 组合 R4 native
owners，拥有 device/layout/graphics 或 compute state 的精确身份。GPU 引用清偿之前不得
销毁其 PSO。目录不拥有提交线程、FrameLoop 或 Material；不引入全局 Registry 或第二 Parser。

累计资格视图固定为 `docs/render-v2/F3_CAPABILITY_STATUS.md`，后续授权阶段更新同一文件。
它只索引冻结 CSV 原编号、精确 I/V、报告/证据、资格类别和未完成项，不复制架构规范。
报告仍是资格事实依据；历史 PASS 不自动升级为新版整行 PASS。当前未提交 F3 仅登记
IN_PROGRESS / NOT_QUALIFIED。最终 V 仍只新增验收报告；视图在 I 中冻结，使用报告路径与
Git 身份解析规则引用将来的 V，避免为了填写自身 SHA 修改锁定 I 或夹带 V 更新。

目标建立唯一的、自洽的冷态管线：

```text
C++ LUX_PASS_PARAMS / LUX_RESOURCE
          │  existing lux-cxx / PassSchema IR / Stage-specific declarations
          ├───────────────────────┐
          ▼                       ▼
F2 LogicalGraphPlan        .lglsl emitter → glslc → original SPIR-V
(resource ranges,           (real stage-specific shader, original reflection)
 producer versions,
 effects/scope)
          │                       │
          └───────┬───────────────┘
                  │  + owner-provided FULL shared descriptor shapes
                  │  + native queried DeviceCaps + pipeline/target signature
                  ▼
          LayoutPlan (唯一最终 set/binding/PC/owner/flags 权威)
                  ↓
          Cold SPIR-V pairwise relocation → spirv-val → re-reflection
                  ↓
          VkDescriptorSetLayout / VkPipelineLayout / ShaderModule
          VkComputePipeline / VkGraphicsPipeline + typed binding recipes
                  ↓
          Move-only complete native candidate (RAII, rollback-safe)
                  ↓
          F3-only real GPU fixture: descriptors → draw/dispatch → readback
```

五个内部步骤、**一个正式 F3 阶段**：

- **F3.0：上游合同完备化。** 整数/浮点 Color Clear 的唯一值模型；Shader Include/自动声明合法顺序，Meta/glslc/SPIR-V 真实验收。
- **F3.1：完整 LayoutPlan。** Typed Owner Shapes、共享 Set 完整形状、资源用途/数组/Stage/PushConstant 与设备预算；单一最终位置权威。
- **F3.2：SPIR-V 重定位与再验证。** 成对修改 Set/Binding、处理不支持格式的明确拒绝、Shader/Graph/Layout 对账及冷态缓存身份。
- **F3.3：真实 Native Descriptor 和 Graphics/Compute Pipeline。** 扩展 R4 RAII 对象、完整 Descriptor 更新和候选事务，冷态生成稳定 Binding Recipe。
- **F3.4：实际 GPU 数值/故障/性能验收。** 真实采样、SSBO、Graphics、共享 Owner Set、整数 Clear、限制/错误注入及恢复。

**F3-PRE-01/02 的旧追踪项在 F3.0 的内部证据通过时标记 `F3.0 AUTHOR CONTRACT PASS`，不另创建一个 F3-PRE 验收提交，也不在此停下来等待新的用户授权。** 最终 F3 必须同时通过 F3.0–F3.4。若整轮完成条件未满足，结果只能为 `PARTIAL/FAIL`，即使 F3.0 已通过也不算 F3 PASS。

### 严禁混淆的三种完成状态

| 可交付 | 当前 F3 必须完成 | F3 完成时依旧不得声称 |
|---|---|---|
| Shader/C++/SPIR-V 单源合同 | YES | —— |
| Native LayoutPlan、Descriptor、Shader/PSO 编译与原生 shader 执行 | YES | —— |
| 通用 Native VulkanGraphCompiler C10/C12/C13、Alias、Graph record、跨队列统一调度、完整 F4 Executor | **NO** | `NATIVE_RENDER_GRAPH=NOT_IMPLEMENTED` |
| 真正 N Scene/N View/N Target 唯一 Runtime/FrameLoop | **NO** | `RENDER_RUNTIME=NOT_IMPLEMENTED` |
| F6 Plugin/全 Legacy Feature/产品切换 | **NO** | `V2_PRODUCT=EXPECTED_UNAVAILABLE` |

## 2. 源码和现有机制的真实边界（必须复用）

### 2.1 F1/F2 已有作者与逻辑资产

- Graph：`modules/function/render/graph/include/lux/engine/render/graph/{Authoring,Schema,Builder,Definition,Plan,Bindings}.hpp`、`src/{Builder,Definition,Plan,Bindings}.cpp`，以及 `graph/cmake/{PassSchema.py,RenderGraphCodegen.cmake,pass_schema.template}`。
- 中立契约：`modules/resource/description/include/lux/engine/description/{Image,PassContract,LayoutContract,ShaderInfo}.hpp`。`rdesc::ETextureFormat` 是唯一纹理格式权威；`PassShaderContract` 与 F1/F2 生成 Schema 是字段真相。现有 `LayoutContract.hpp` 包含历史 canonical slot/owner 的有效输入，但**不能让其旧静态槽表与新 LayoutPlan 成为两份独立最终权威**。
- Toolchain：`engine/toolchain/shader/src/lglsl/LglslEmitter.cpp`、`src/{PassValidation,SpirvReflection}.cpp` 及公开 API。其离线生成、编译和反射链继续是唯一实现；不能增加 Parser、反射注册表或每帧编译服务。
- F2 只读逻辑结果：`compileLogicalGraph() → LogicalGraphPlan`，其中 versions/ranges/scope/culling/identity 是 Native 编译输入；`diagnosticDigest()` **绝不是 Native Cache Key**。

### 2.2 R4 真正已有的 Native Foundation

必须查看并直接复用：

- `modules/function/render/vulkan/include/lux/engine/render/vulkan/device/Device.hpp`：`VulkanInstance`、`VulkanDevice`、`VulkanAllocator`；真实 device properties/capability 查询基础。
- `.../descriptor/Descriptors.hpp`：`DescriptorSetLayout`、`DescriptorPool`；现在主要具有 `writeStorageBuffer()`，**尚不等于完整 Descriptor 系统**。
- `.../pipeline/Pipeline.hpp`：`ShaderModule`、`PipelineLayout`、`ComputePipeline`；**尚缺完整 Graphics Pipeline** 与所需布局能力。
- `.../transfer/{Submission,Transfer}.hpp`、`.../retirement/Retirement.hpp`、既有 Buffer/Image 及原生测试。用其建立一次性 F3 fixture 提交与 readback、生命周期验证，不重复创建 Device/VMA/Queue/Retirement owner。

冻结 Legacy 的 `render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/pipeline/{SpirvPatcher,EngineSetShapes,StandardPipelineLayoutBuilder}.hpp`、`.../graph/LayoutPlan.hpp` 和对应原 `.cpp` 仅为**历史算法/行为参考**。新版分支尚无一个可直接调用的正式 F3 Patcher 或 LayoutPlan：允许根据实际兼容范围实现新模块内冷态逻辑，**禁止编辑、编译链接、复制粘贴后以旧 Legacy 实现充当 V2 生产依赖**。记录来源及差异。

### 2.3 依赖组织的正确选择

`render_graph` 只能向下依赖 Core 与轻量中立 Header，绝不可依赖 Vulkan。F3 Graph-aware 编译可以放在同一个 `render_vulkan` 所属模块内的专用子目标/源目录，按需要 `PRIVATE` 消费 Graph/Neutral Shader Metadata/R4 Native 基础；**不得因此把 R4 Foundation 公共组件整体变成必须链接完整 Graph/Toolchain 的目标，更不得建立第二套 Native Foundation**。由实际 CMake File API + link/source closure 证明依赖层次。若选择单目标扩展而不能保留已冻结的 R4 面向 Core 消费者的接口/回归边界，须在施工前提出最小修订并 STOP。

## 3. F3.0 — 上游作者合同内部强制门禁

> F3.0 是 F3 中首项必须验收的工作，**不是一个独立审批阶段**。如未满足其全部输入合同，后续步骤不得依赖错误的上游数据继续施工。F3.0 需要完整测试结果与生成物哈希，计入最终 F3 I/V 的同一证据归档。

### F3.0-A — 浮点、SINT、UINT 的严格 Color Clear

**原问题：** `Attachment::clear`、`CapturedFieldBinding::clear`、`GraphPassInvocation`/`ColorClearValue` 只具有四通道 float 清色语义；但真实 Color target 格式包括 `RGBA8_SINT`、`RGBA8_UINT`、`R32_UINT` 等整数格式。只在 Vulkan 转换时加入 union 并不能恢复上游已丢失的信息。

**正式目标**：将真实类别设计为一个封闭、类型安全的值类型；例如：

```cpp
struct FloatColorClear { std::array<float, 4> values; };
struct SintColorClear  { std::array<std::int32_t, 4> values; };
struct UintColorClear  { std::array<std::uint32_t, 4> values; };
using ColorClearValue = std::variant<FloatColorClear, SintColorClear, UintColorClear>;
```

示例说明语义；API 拼写可以遵循既有规范调整，但必须满足：

1. **唯一格式分类**在现有 `rdesc::ETextureFormat` 的中立辅助算法中给出适当的 `FLOAT / SINT / UINT / DEPTH_STENCIL / INVALID` 逻辑分类；不新增第二格式枚举、全局 Registry 或 Vulkan-specific 判定。UNORM/SNORM/SRGB/浮点归属 Float Clear；有符号整数归属 SINT；无符号整数归属 UINT；`D24_UNORM_S8_UINT` 等深度模板绝不可因名字含 `UINT` 而变成 Color Uint。设备实际格式能力由 F3 Native 另行查询。
2. Graph 作者 `Attachment`、Generated PassSchema 捕获、`CapturedFieldBinding`、`RenderGraphDefinition` 验证、`LogicalGraphIdentity`（仅结构部分）、`makeGraphInvocationData()`、`GraphPassInvocation` 和 `FrameGraphBindings::validate()` 必须是一条准确的数据链；C++20 `variant` 本身不是最终质量证据，必须从真实作者 Params 走到实际 native 消费者。
3. `ELoadOp::CLEAR` 时实际清色类型须与目标 Color 格式匹配。`LOAD` / `DISCARD` 不应因无意义的默认 Clear 值而改变合法性或计划身份。Depth/Stencil 的 `depth`/`stencil` 清除保持独立，不假装是 Color 四元组；动态值在 Native 转换时按真实类别填充 `VkClearColorValue.float32/int32/uint32`，不能把整数转浮点再转回，也不能用无证据的 reinterpret。
4. **结构与值分离**：同一 Attachment 格式/载入语义与 Shader 接口不变时，两 View/Invocation 改变同一类别的 Clear 数字仍可共享 LogicalPlan；真实更新须出现在各自 Frame Invocation / Descriptor/Record Recipe 消费中。格式类别变化必须触发正确的布局/PSO key 校验；无效 Clear 类型在 CPU 冷态/绑定验证就拒绝。
5. `Attachment` 的 C++ 对象布局变化后，必须让生产 lux-cxx → IR → PassSchema 真正重新生成 sizeof/align/offset；如 IR-lowering 需要修改，只扩展现有生成器接线和资源捕获，无手写 fixture specialization、无第二 C++ Parser。任何其他资源/Shader ABI 变化必须逐项说明。
6. 最少用例：`R32_UINT` 的 `16777217u` / `0xffffffffu`、至少一个 RGBA Uint、一个 SINT 的 `-1` 与边界、Float/HDR/sRGB；错误的 SINT/UINT/Float 互配、Depth 伪 Color、未知格式、错误范围均拒绝。**必须通过真实 GPU Attachment `loadOp=CLEAR` → readback**，不是只拿 `vkCmdClearColorImage` 代替 Attachment 清屏；F3.0 先锁定 Authoring 验证，F3.4 完成 GPU 数值证明。

### F3.0-B — Shader Include / Generated Declaration 正确排序

**原问题：** `emitPassGlsl()` 对开头 `#include`/`#define`/`#extension` 的简单拼接，无法同时保证「生成字段依赖 include 的类型」与「include helper 读取生成字段」这两种顺序。测试不能只检查字符串位置。

允许在**现有 `.lglsl` emitter** 扩展一个明确的轻量作者指令，例如 `//! lux-pass-declarations` 标记**生成字段插入点**（具体拼写冻结在 F3 文档中），但不得新增第二套预处理器/Parser/Reflection。规则必须清楚：

```text
#version / 必需 #extension
→ 生成声明依赖的类型与宏（前置 include/define）
→ 唯一 PassSchema 字段生成插入点
→ 使用生成字段的 helper/include
→ Shader body / entrypoint
```

- `#version` 首个有效指令；重复、晚到版本声明、顺序不合法的扩展必须带源位置或清晰错误。`#extension GL_GOOGLE_include_directive`、传递 include、宏/条件仍交给真实 glslc 预处理；不能通过扫描 include 文件来发明另一套依赖推断。
- 对未显式标记的旧合法输入，只做有定义且安全的兼容缺省行为；无法判断前置类型还是后置 helper 时明确要求指令或给出错误，不能靠路径名猜测。记录所选 Stage 及原始 Shader Hash、生成 GLSL Hash、SPIR-V Hash、Meta/Schema revision。
- 同一份 PassParams 的 Vertex/Fragment/Compute `stage_declarations` 必须只包含目标 Stage 需要的字段；共享字段在原 provisional canonical slot 保持一致，不能用 F3.0 排序修复破坏 F1-FIX 的多 Stage 合同。
- 正例至少包含：原字节一致的 Legacy tonemap 输入；前置类型/宏 include；后置 helper 实际读取生成的 sampled、SSBO/UBO、PushConstant；多层传递 include 与受条件宏控制的合法分支；Vertex/Fragment 共享资源。全部走 **生产 emitter → glslc → spirv-val → reflection → PassSchema validator**。
- 负例至少包含：重复/晚到 `#version`、非法扩展位置、生成字段与 include 相互依赖无法满足顺序、两次插入标记、Stage 错误、手写 `set/binding` 逃生、helper 引入未声明资源、类型/数量/访问不匹配。不能只做 `find()` 的字符串测试、不能手写最终 GLSL 或依赖已有编译产物绕过真正生成。

### F3.0 内部门禁

保存上述所有正反例原始命令和产物。若未通过，停止依赖其输出的 Native 编译；允许在本次 F3 授权内修复后重新运行，不需再请求一次新的 F3-PRE 人工授权。**禁止为了处理 F3.0 缺陷修改 F2 的 C3 版本/资源 Hazard/C7 Scope 语义。**

## 4. F3.1 — 唯一 LayoutPlan：Owner Full Shape、设备限制与 ABI

本步骤交付一个冷态计算出的、结构完整、可以被 R4 Native 直接消费的 `LayoutPlan`。其输入来自真实 Schema、已验证的 SPIR-V 接口和传入的**Owner 全量契约**，而不是按每个 shader 反射子集拼 Layout。

### F3.1.1 输入和数据责任

- `PassSchema`：字段路径、资源 Role、Owner、更新频率、Stage、数组、Image dimension/format、range、ABI offset/stride/size、Shader provisional logical descriptor identity。
- `LogicalGraphPlan`：被裁剪/存活 Pass、显式结构绑定、Attachment 类型/阶段、进口/出口作用域；不是 GPU Native lifetime 证明。
- `OwnerShape`（命名仅示意）：由实际 Engine/Scene、Feature 或 Pass-local **声明完整 Descriptor Set Shape** 的中立值记录。包含稳定 owner identity、字段完整集合、Descriptor Type/Count/Stage/flags 与布局版本。一个 Shader 使用 Owner 字段子集不得新建不兼容 subset Layout。**F3 尚无 F6 真正 Feature owner，允许 fixture 注入具体 Owner Shape 来验证 LayoutPlan，不得伪造已经存在的 Feature/Scene 生命周期或 Runtime 注册系统。**
- `DeviceLayoutCaps`（命名示意）：F3 自己从真实 `VkPhysicalDeviceProperties2`、Feature 查询、实际 enabled capabilities 提取的设备侧冷态值。不要擅自扩大 R1 Core 的小型 `DeviceCaps` 结构，也不能猜常量认为所有设备有 128B PC / 4 sets / 任意 descriptor indexing。

### F3.1.2 必须输出

- 有唯一完整字段映射 `LogicalFieldIdentity → (owner, set, binding, descriptorType, arrayShape, stages, flags, dynamicIndex)`，以及真实 `VkDescriptorSetLayoutBinding`/binding flags、per-owner Full Shape、`VkPushConstantRange`、动态 UBO/SSBO offset 顺序、Shader stage layout compatibility、Layout Signature 和独立诊断。
- 多逻辑 Owner 可以在 **设备实际上限内** 合并成少量物理 Vulkan Sets；不保证一个 Owner 就是一个固定 Set。两个 Feature 都使用历史 canonical `set=1` 不会因“同一数字”而混成同一个资源 Owner。
- 集成 Stage/Vertex/Fragment/Compute 合并后的 Descriptor Count，运行时数组/Descriptor Indexing/PartiallyBound/VariableCount/UpdateAfterBind 必须按**真实启用的设备特性和真实布局规则**核算。不支持时选择已定义且功能等价的合法编译变体，或明确返回可诊断的 `unsupported/limit`；不能静默删 Binding 或绕过设备限制。
- UBO/SSBO Range、alignment/dynamic offset、Storage Image format、sampler/pairing、attachment role、PushConstant 范围/Stage 与 Schema/反射逐项对账。`maxBoundDescriptorSets`、per-stage descriptor counts、per-set 约束、`maxPushConstantsSize`、`minUniformBufferOffsetAlignment`、`minStorageBufferOffsetAlignment`、动态 offset 数量、variable count 限制等必须从实际 device query 得到。必要时允许有界“人为降低限额”的纯 CPU 负例，但不能把它冒充真实设备支持。
- 一份真正共享 Set 的两个 Shader，只读取不同字段子集时：二者的实际 PipelineLayout 中该 Set 的**完整形状相同且可兼容**，必须有真实 GPU 使用场景。应同时测试同名字段不同类型、数组 Count 不一致、不同 Owner 同槽冲突、重复位置、空 Shape、非法扩展标志。

### F3.1.3 API / 类型约束

优先使用简单冷态值、free functions、短事务 Builder，输出**完全验证**的 owning LayoutPlan。不要创建 `LayoutService/ShaderLayoutManager/DescriptorRegistry`。`rdesc::LayoutContract.hpp` 是历史/中立输入，不得继续为其旧 slot 表保留另一套最终权威；由 LayoutPlan 单点分配和分发最终物理位置。公共 Graph/中立 Description 头不引入 `vulkan.h`；Vk-specific Shape 仅在 Native 模块私有层。

## 5. F3.2 — Cold SPIR-V 成对重定位与反射校验

目标不只是改两个 SPIR-V 数字，而是建立可审计的**原始字段身份→最终 LayoutPlan→真实二进制**闭环。

- 先根据预重定位时记录的 `(variable identity, original set, original binding, role/type/count/stage/interface)` 与 PassSchema/原始 Reflection 对账，再一次性从同一 LayoutPlan 分配最终位置。重定位以**成对 DescriptorSet/Binding** 操作为单元：不能受 Decoration 遍历顺序影响，不能只改 Set 不改 Binding，不可误把旧的物理 `(set,binding)` 当逻辑 Owner Identity。
- 检查 SPIR-V Header/Instruction 长度与 version、`OpDecorate` 重复、同 ID Decoration 冲突、missing Set/Binding、unknown variables、数组/类型/访问差异。对 grouped decorations / decoration groups / alias / multi-entrypoint 等**实际不支持的输入**明确记录受支持格式并拒绝，不得声称任意合法 SPIR-V 都能 patch。根据需要借鉴冻结 Legacy `SpirvPatcher` 算法，但绝不修改或链接冻结实现。
- Patch 后强制 `spirv-val` + 实际 SPIRV-Cross reflection（或使用生产 validator 的等价方法），逐字段验证完整 `LayoutPlan` 位置、Stage、Descriptor Count、Push Constants、block offsets、storage stride。缓存记录原始 SPIR-V、Patch 后 SPIR-V、Schema/Layout revision、Device/layout variant 的 hash / checksum，测试精确产物。
- `record()`/每帧 Draw **不做** Shader compile、IR 解析、SPIR-V patch、反射或 Layout 搜索；这条链必须是冷态的。
- 必须有 Vertex+Fragment 共享字段位置一致、两个 Shader 使用共享 Owner 字段子集、单 Pass-local 多资源、offset/count 错误、重复/缺失 Decoration、错误 Source/Stage、unsupported SPIR-V feature 等正反例，并保留用于诊断的映射表。

## 6. F3.3 — 复用 R4 的 Native Descriptor/Pipeline 与事务所有权

### F3.3.1 正式 Native 能力

- 扩展现有 R4 `DescriptorSetLayout`、`DescriptorPool`、`PipelineLayout`、`ShaderModule`、`ComputePipeline` 所缺的必要功能；新增真正的 `GraphicsPipeline`（move-only RAII），支持真实 Vertex/Fragment Stage、vertex input、primitive topology、raster、depth/stencil、color blend、MSAA、attachment format/signature、dynamic rendering/renderpass 的可用合法变体；不能把 Compute Pipeline 冒充图形流水线。
- 扩充 Descriptor 写入覆盖 F3 实际 Shader 用途：separate SampledImage/Sampler、combined image sampler（实际需要时）、StorageImage、UniformBuffer、StorageBuffer、数组、多 Set Owner、动态 Offset、Push Constants、合法的 optional indexing flags。实例必须完整符合对应 Layout；错误边界前置验证而非交给 Vulkan Validation 报成功。
- PipelineLayout 创建支持完整 owner set layouts + stage-specific PushConstantRanges。PSO Native identity 至少含完整 logical structural identity 的相关签名、**真实 Shader binary/interface hash**、Layout Signature、Device capability variant、actual target format/sample count、vertex/raster/depth-stencil/blend/topology/specialization state。F2 `diagnosticDigest()` 不得作为缓存键；冷态若提供预筛哈希，命中仍必须 exact/collision-safe 校验。
- 编译结果要直接提供固定 typed Descriptor/Push/Pass binding recipe：Resource slot、owner、set/binding、array element、动态 offset 顺序、Stage、Native layout/PSO。热路径没有字符串/RTTI 查询或临时 set/binding 分配。Graph Invocation 中实际 Clear/Scalar/Sampler、Camera/History/backing 独立且不混入稳定逻辑编译身份。

### F3.3.2 候选事务与故障

- 采用 `prepare fully formed candidate → validate → publish only complete result`。候选的单个 DescriptorSetLayout / PipelineLayout / ShaderModule / PSO 建立失败时，局部 RAII 正确释放已创建但未发布资源；已有 last-good pipeline 始终可被继续合法使用。
- 对任何**已提交 GPU 工作**引用的旧 Pipeline/Set/Layout/Backing，必须保留在途 pin，并仅依据 R4 Submission/Retirement 的实际完成证据销毁。失败不得 `vkDeviceWaitIdle` 作为常规销毁策略，不得用 Frame Index 推算 GPU Completed；Device Lost 走 terminal session 故障路径。
- 旧 R4 Foundation 仍是一份 Device/VMA/Queue/Retirement Authority；不能建立 `VulkanDevice2`、`GraphicsManager`、`IBackend`、第二个资源缓存 Owner。建议新增的 F3 candidate 只用**组合**持有已经合法的 R4 RAII 值，不重复发明 `Vk*` 所有权包装类型。
- Descriptor 更新不得覆盖仍由 GPU in-flight 使用的非 UpdateAfterBind Set；候选/Frame-slot 更新规则必须有明确的 ownership、pool 生命周期与合法更新时点。Device/Queue/Pool/Shader/Pipeline 的析构顺序通过 RAII/真实提交证明落实，不靠多个 `closing/initialized` bool 补偿。

### F3.3.3 CMake 依赖设计

允许在 `modules/function/render/vulkan/**` 下组织 Native Shader/Pipeline Compiler 的生产源码和真实测试，优先保持 R4 基础组件面向 Core 的已有公共依赖不变。需要消费 F2 `LogicalGraphPlan` 时可建立一个**专门的 Shader/Pipeline 编译目标**（并非第二 Foundation），其作用只限 F3 编译资产与 recipe；必须提供实际消费者、CMake File API/compile commands/link closure，并证明无需将离线 Compiler/Shader Tools 链入 Frame record 热路径。禁止同时运行两套 Descriptor/Pipeline 编译体系。

## 7. F3.4 — 真正 GPU 验收（不得以 CPU mock 替代）

本阶段必须在**真实 VulkanDevice**上创建 DescriptorSetLayouts、PipelineLayouts、ShaderModules、Compute/Graphics Pipelines，使用一份已验证的 `LayoutPlan` 和实际 descriptor recipe，完成真实 GPU 工作和 readback。可以用 R4 queue、已存在 Buffer/Image、一次性 CommandBuffer/Barrier helper 完成专用 Test Harness；这**不等于**也不能代替 F4 的通用 NativeGraphRecorder/BarrierProgram。

### F3.4.1 必须的 Golden GPU 工作负载

| ID | 工作负载 | 正确性 oracle / 反作弊 |
|---|---|---|
| G01 / W01 | 同源生成的 Fullscreen Tonemap + Blur + Composite；真实 Sampled Texture/Sampler、多输入与 Shader 变体，float/HDR/sRGB GraphicsPipeline → Color Image → Readback | 分别及串联对照 CPU 图像 reference；Blur 读取邻域，Composite 消费不同输入；改变输入/参数/变体须改变期望输出，不得用常数或仅 Tonemap 代替 W01。测试专用单 Queue 录制，不建立通用 Graph Recorder |
| G02 | Compute SSBO：只读/读写 SSBO + StorageImage/Buffer → Readback | 验证真实输入值、实际 offset/stride、不同 array element、descriptor writes 与 GPU 写回；不允许只用一个 `writeStorageBuffer` 证明全部格式 |
| G03 | Vertex+Fragment：共享 Scene/Feature Owner Full Shape，两 Shader 消费互补子集 | 两份 PipelineLayout 的共享 Set 完整 shape 一致，实际 Shader 读取不同字段并通过 GPU 数值 oracle；Shader 反射子集不能私自决定 Owner Layout |
| G04 | Pass-local 多描述符：Sampler、Sampled/Storage images、UBO/SSBO、独立 Set、动态 offset、PushConstant | 真实 GPU Output 与预期相符；手工写错 set/binding 或不匹配数组应冷态拒绝 |
| G05 | **整数 Attachment CLEAR**：`R32_UINT`、至少一项 RGBA UINT、SINT 负值，真实 `loadOp=CLEAR` 后 readback | `16777217u`、`0xffffffffu`、`-1` 等数值无浮点精度损失；不能用直接 `vkCmdClearColorImage` 代替对 Attachment Clear 的验证 |
| G06（GPU PSO 状态） | Depth/Stencil、MSAA/color blend/resolve 的测试专用 native workload | 证明 Graphics PSO 状态和真实输出；不实现通用 Graph Resolve、跨 Pass Barrier 编译或资源调度。不支持的设备变体明确拒绝/正确 fallback |
| G07（GPU 生命周期） | 最小 Candidate rollback / Shader replacement / in-flight pin，复用 R4 submission/retirement | Shader/Layout/PSO 候选失败保持旧输出；成功替换后旧 PSO 等待真实完成证据。不得建立 F5 Hot Reload 服务或 FrameExecutor |
| G08（冷态负例） | 设备 Budget：set 数、per-stage descriptor、PC 大小、动态 offset/对齐、unsupported indexing flags | 实际设备查询与 CPU 拒绝；人为降低快照不冒充物理设备，不要求对非法输入 GPU readback |
| G09（冷态负例） | SPIR-V 重定位：重复/缺失 decoration、字段类型/数组数、Stage/Shader 接口不一致 | Cold fail 且具体错误；有效 patched SPV 通过 spirv-val/重反射并供 G01–G07 执行，非法输入不得提交 GPU |

还须保留原有 R4 Buffer/Image/Compute readback、Device Lost/Fault、Retirement 等真实测试。开启 `VK_LAYER_KHRONOS_validation` + synchronization validation，**GPU Validation Errors = 0**；保留原始 Validation 输出，不得删警告、禁止一律关闭 validation 或以 return-success 跳过可选 GPU 测试。

### F3.4.2 F3 与 F4 测试界限

F3 Test-only 一次性 Buffer/Image layout transitions、CommandBuffer 录制、单 Queue submit/readback 用于证明 Native Shader/Pipeline。**不允许**由此声称 F4 的跨 Queue schedule、RAW/WAR/WAW 全图同步、Aliasing、2/3 FIF PhysicalResourcePlan、Multiview Executor、Target Acquire/Present、通用 Graph record/submit 已完成。F3 只承诺到 Native Pipeline/Descriptor 资产与 fixture 实际运行；F4 仍需完整实现 C10/C12/C13 并新设 GPU oracle。

设备不支持的可选特性，必须记录 feature/property/extension/format 查询的真实输出，选择已经定义、功能正确的基础 fallback；如该 F3 必需能力不具备且无法测得真实正例，则 F3 **PARTIAL**，不能靠模拟输出宣布 PASS。对于高阶 descriptor indexing/update-after-bind 单独记录 Supported/Unsupported/NOT_RUN，基础 Descriptor/GPU 工作负载必须跑通。

## 8. C++20 类型/函数/所有权设计门禁

遵守 FINAL 12，按**职责**而非类名组织：

1. **纯值**：`ColorClearValue`、Owner Descriptor Shape、LayoutPlan 值、Shader Identity、Device Layout Caps、BindingRecipe/PSO description 只表达结构事实；不同互斥数据用可说明正确性的 `variant`/强类型。禁止再造重叠的 `kind + 两份 payload`。
2. **无状态算法**：`buildLayoutPlan(...)`、`relocateSpirv(...)`、`validateShaderLayout(...)`、`explain...(...)` 之类为 free functions，按实际代码库命名。不得为了调用它们创建持久 `LayoutService/ShaderCompilerManager`。
3. **短生命周期构造**：只在真正涉及候选事务和可变装配时使用 Builder，消费后只产出完整 plan/candidate。复用 F2 `GraphPassParameters<T>` 与同源生成；不要为一个算法再发明虚 Feature interface。
4. **RAII Native Owners**：旧 R4 ShaderModule/DescriptorSetLayout/PipelineLayout/ComputePipeline 和新增必要 GraphicsPipeline 的生命周期唯一、move-only。`RenderResult<T>` 出错返回具体错误；OOM 依据仓库统一 fatal policy，不将分配失败伪报业务错误。
5. **热路径**：固定容量 warm 操作 0 首方堆分配、无 C++ string/RTTI/全局映射/每 Pass `std::function`、不生成 Shader/Layout/PSO。`noexcept` 不能被误理解为不分配；cold 创建允许实测分配。
6. **ABI**：只允许 C++20，使用现有 `cxx::expected`。公共 Graph/Description API 不 include Vulkan/Scene/Runtime；Plugin 外部 C ABI/code pin 在 F6 单独验收，不能在 F3 通过 header 安装同步假装它已经兼容。

每个新增公开/私有非平凡类型提交 Type Responsibility Inventory：`name / public-or-private / Value|Algorithm|Builder|RAII Owner / 真正 owner / borrow & thread / cold-or-hot / failure / 依赖 / 为什么更简单类型不足 / ABI 与测试`。如新增目标/缓存/类，必须附性能、消费及最小性论证。

## 9. 文件授权：唯一 F3 实施范围

F3 作为一个阶段同时授权必要的上游合同和 Native Shader/Pipeline 修改；**不要再把 F3.0 当作需要新批准的阶段**。

### 9.1 ALLOWED（必须限制在实际所需最小文件）

- `modules/function/render/graph/include/lux/engine/render/graph/{Authoring,Schema,Definition,Plan,Bindings}.hpp` 与 `src/{Builder,Definition,Plan,Bindings}.cpp`：**仅** F3.0 Clear 类型/捕获/实际 Invocation 语义、F3 对接所必需的中立字段；`modules/function/render/graph/test/**` 及必要 `graph/cmake/PassSchema.py`/模板。**C3 版本算法、C5 Culling、C7 Scope、F2-FIX 可达性逻辑默认 READ-ONLY。**
- `engine/toolchain/shader/**`：现有 emitter 的声明顺序、Stage、SPIR-V relocation/reflection/验证所需最小扩展、必要 tool/test/CMake。离线编译不进入生产热路径。
- `modules/resource/description/include/lux/engine/description/{Image,PassContract,ShaderInfo,LayoutContract}.hpp` 的确有需求的**中立 metadata/value**调整与对应已有实现/测试；不引入 Description Math/Script 等聚合链接依赖，不允许再定义一套 Texture Format 权威。
- `modules/function/render/vulkan/{include,src,pinclude,test,cmake}/**`、`modules/function/render/vulkan/CMakeLists.txt`：R4 Foundation **增量能力**及 F3 真正 Native Shader/Layout/Pipeline/Descriptor 子模块和测试。旧 API/Owner/Retirement 回归必须保留，不造第二 Vulkan Foundation。
- `cmake/render-v2-bootstrap/**`：仅真实 F3 targets、fixture、Codegen 接线、资格证据脚本。
- `docs/render-v2/F3_*`：本工作单、Implementation/Type/Algorithm/ABI/GPU oracle 清单、资格验收报告和阶段证据索引。

### 9.2 CONDITIONAL（先证据，后批准）

- 除上述明确授权内容以外，任何 Graph 生产接口修改、Core/Transport 头修改、Meta Generator (`lux-cxx`) 语法/二进制修改、Root CMake/package 安装接口更改，以及历史 Final 设计冻结决策冲突：**必须先给出实际无法在现有目录完成的编译/ABI/功能反例、最小 diff 与替代方案，并 STOP 请求用户单独批准**。不能为了编译通过扩大权限。
- 跨模块共享 Shape 的 neutral metadata 若确实需要 Description 新文件，必须限定于纯字段/值/算法、证明无 Vulkan private include 和第二 Layout Authority；不得创建运行时 Registry。

### 9.3 READ-ONLY / FORBIDDEN

- `modules/function/render/core/**`、`modules/function/render/transport/**`（含 F2 已授权对齐格式）、`render_legacy/**` 全部 719 冻结源文件及 Guard。
- `docs/render-v2/final/**`、R0–R4/F0–F2-FIX 所有历史实施及验收报告；不得回写旧 PASS/PARTIAL/NOT_RUN。
- 原用户 worktree 六处未提交改动，Scene/ECS、Editor/UI、Feature 业务、`render_runtime/**`、Root/Product 的现有正式渲染器、其它 Platform renderer。
- **禁止** F4 完整 VulkanGraphCompiler、通用 transient allocator/alias、cross-queue Graph schedule、Native Graph Recorder/BarrierProgram、第二帧循环、多 SceneRuntime/FeatureManager，以及 H1–H6/Legacy 功能迁移。

## 10. 阶段验收：先内部合同，再完整 F3 资格

### 10.1 强制运行矩阵

**原有 124 项 CTest 名称和实际义务全部保留**。禁止只保留名字、改测试断言令旧错误合法、为 green 删除真实 GPU/负例测试。新增测试的具体数量由真实功能决定，但必须覆盖下列维度：

| 类别 | 通过要求 | 失败即阻断 |
|---|---|---|
| F3.0 Typed Clear | C++ compile+runtime 正反例、真实 Meta 捕获、Invocation 隔离、SINT/UINT 无损 | 整数经 float 转换、Wrong Kind 接受、旧 Depth/Stencil 回归 |
| F3.0 Emitter | 前置类型/后置 helper/多层 include/条件宏、Multi-stage，glslc+spirv-val+reflect | 只比较字符串、重复 `#version` 被接受、手写物理 Set 绕开 Toolchain |
| F3.1 LayoutPlan | owner Full Shape、Stage 合并、arrays/flags/PC/dynamic offsets、设备真实限制 | subset Shape 冒充全量、Owner 混淆、超过 limit 静默截断 |
| F3.2 Relocation | 原/最终 SPV hash、成对 Decoration、重新反射与 SPIR-V validation | mismatch 未拒绝、只 patch Set、record 时 patch |
| F3.3 Native | Graphics/Compute/Descriptors/Pool/Layout RAII、create failure/rollback/last-good | 未建真实 PSO、Descriptor 少种类、旧对象提前销毁 |
| F3.4 G01–G07 / W01 / W15 | 真实 GPU draw/dispatch/readback 与生命周期证据，Validation+SyncValidation 0 errors | mock 常数输出、遗漏 Blur/Composite、只跑 R4 smoke、没有真实 readback |
| F3.4 G08–G09 | 实际设备限制查询、冷态拒绝、SPIR-V 原/最终验证 | 伪造 GPU readback 类别、将非法命令提交 GPU |
| Graph/F2 回归 | 原 300 随机 + 700 mixed oracle + C7 ScopeProof + F2-FIX 性能/Binding | Graph C3/C7 被 F3 改坏、旧测试改语义 |
| Public/闭包 | C++20 公开头独立 TU、Concept 负例、File API/compile/link/codegen closure | Graph include Vulkan、Script/Math 聚合、Shader Tool runtime 耦合 |
| Sanitizer | 普通与完整 ASan 双轮 full build，第 2 轮 no-work；全量 CTest | 环境依赖不匹配被隐藏、用关闭 STL annotation 换绿 |
| 源码保护 | Legacy、Core、Transport、历史报告、用户六处修改原样 | 越界提交、历史资格被重写 |

### 10.2 时间/内存/分配/性能

- **稳态 Binding 必须保持零首方 C++ heap allocation**。与 F2-FIX 同功能同设备同负载交替配对 7 组×每组 1,000,000 操作（或更多），10k 预热，保留 p50/p95/max/样本；不可解释的 >5% p50/p95 回退 STOP。
- 分开提供 F3 冷态 `layout-compile`, `spirv-relocate+validate`, `native-pipeline-create`, `descriptor-allocate+write` 的样本/CPU p50/p95/max/alloc_count/bytes。与不存在的旧 F3 相比没有等价 baseline 时只能建立新 baseline，不得编造提升百分比。
- 真实 GPU readback 数值/Validation 是正确性门禁；GPU timestamp/record CPU/VRAM peak 可按 fixture 与硬件能力记录基线。不允许以减少工作量、更低采样数、空 Pipeline 或删 Shader/Descriptor 字段换取性能数字。
- F2-FIX 的 64/128/256/512 Pass 冷逻辑编译回归需要保留（或等价测试）；F3 只应有界扩展增量编译成本，不能变成每帧完整 Graph 重编译。
- Hot path 严禁 lazy shaderc/spirv cross/PSO/layout/descriptor pool/new dynamic string lookup；保留真实容量/池上界和设备预算。

### 10.3 独立 I/V 验收流程

1. 在**独立施工 worktree** 完成 F3.0–F3.4，提交最终 Implementation I（可包含此前清晰有序的中间施工提交；最终 I SHA 必须固定）。确保候选不是只跑 CPU 测试，GPU G01–G09 的原始脚本和读回证据均可执行。
2. 从最终 I 执行 `clone --no-hardlinks --no-checkout`，detached checkout SHA，源树外 configure/build；普通与完整 ASan 全 `all -j 4 -- -k 0` **分别两轮**，第二轮 `ninja: no work to do`；全量 CTest、真实 Vulkan Validation/SyncValidation、Shader/Meta/Relocation、正负例编译、真实 GPU readback/rollback。
3. 核验 File API、`compile_commands.json`、Ninja deps、每个 native target 的实际 source/include/link、generator depfiles、原始 GLSL/SPIR-V/Relocated SPIR-V/Reflection、设备限制查询、Shader/PSO/Layout 哈希及安装头同步。Graph/Core/Transport/Legacy 不得链接到 Vulkan 或 frozen Legacy。
4. 核验所有保护 Git blob/mode/size、原用户 worktree HEAD/status/diff/untracked 清单和六文件 SHA-256、安装前缀与独立源身份。**三安装前缀 header sync 只证明复制内容，不能冒充 Android/完整 installed SDK 资格。**
5. 将全部命令、退出码、负例编译日志、CTest（包括 verbose）、ASan、GPU Validation/readback reference、CPU/GPU 性能 raw samples、device properties/limits/caps、`VkResult` 注入与资源退休证据，以及证据文件 SHA-256 manifest 归档到源码树外。
6. 如果最终 I 的验收发现实现问题：回到施工树新增**新的最终实现提交 I2**，重新完整独立资格；**不得**在验收 clone 打补丁，也不能在 V 报告提交夹带生产修复。
7. 最后 Verification V **只新增 `docs/render-v2/F3_VERIFICATION.md` 验收报告**。校对远端 HEAD、I/V 亲子关系及范围后推送，立即 STOP，等待用户复审；不自动进入 F4。

### 10.4 必须公开的资格边界

- Linux/Android、完整 installed SDK、外部 Plugin ABI、正式 Runtime/Target、多 Queue Native Graph、2/3 FIF Graph Alias、Legacy Feature parity：没有在本轮真实运行则一律写 `NOT_RUN / NOT_IMPLEMENTED`，不能用 Windows R4 Foundation GPU PASS 补证。
- F3.0 作者合同通过而真实 GPU 数值未通过：`F3 = PARTIAL`，不是 PASS。
- 缺少原生设备正例、任何 GPU Validation **error**、候选失效破坏 last-good、真实整数 Clear 出错：`F3 = PARTIAL/FAIL`，不因为所有 CPU tests 通过而放行。
- F3 之后 `NATIVE_RENDER_GRAPH = NOT_IMPLEMENTED`（F4）、`RENDER_RUNTIME = NOT_IMPLEMENTED`（F5）、`V2_PRODUCT = EXPECTED_UNAVAILABLE`（F11 前）。

## 11. 不可接受的捷径与审查红线

- 新建 `VulkanFoundation2`、`PipelineManager2`、独立 VMA/Device/Retirement 或其他第二 authority。
- 以 shader reflection 的**子集**决定 Scene/Feature Shared Descriptor Set 的完整形状。
- 为 F3 生产写另一个 C++/GLSL Parser 或 F3 专用手写 Descriptor binding 表；不回退到作者手写 set/binding。
- 复用冻结 Legacy 的 `SpirvPatcher` **二进制依赖**或编辑 Legacy 源；允许只以其已证实的机制为来源研究。
- 仅将 `ColorClearValue` 扩展成 Native union，而不修复 Graph/Invocation 原始数据。
- Emitter 通过粗暴挪动全部 `#include`、忽略 `#extension` 或只对 Fixture 原字符串硬编码处理。
- 通过关闭 Validation/ASan STL 注解、删掉错误用例、减少实际 Shader IO 获取 PASS。
- 声称 C9 LayoutPlan=F4 C10–C13 全图 Native 编译、声称 F3 只有 CPU Reflection 就等于真实 Pipeline 已实现。
- 将 `LogicalGraphPlan::diagnosticDigest()` 作为完整结构/Native Cache Key；用 hash 相等代替全量 identity 比较。
- 使用 `vkDeviceWaitIdle` 作为正常 hot replace、View/Scene 销毁或 in-flight retirement 路径。
- 将临时测试 helper / 二次 EventBus / Context / 额外 FrameLoop 放入产品公共 Native API。

## 12. 交付文件与最终 STOP 报告模板

最终工作单与附属文件路径必须在 `docs/render-v2/F3_*` 下追踪，不改 FINAL 主文档：

```text
docs/render-v2/F3_WORK_ORDER.md           # 本工作单，单阶段授权
docs/render-v2/F3_IMPLEMENTATION_INVENTORY.md
                                              # 文件/类型/算法/旧机制复用与依赖证据
docs/render-v2/F3_LAYOUT_CONTRACT.md       # Owner full shapes、实际 Device limits、final placement 规则
docs/render-v2/F3_SHADER_CONTRACT.md       # Include marker、generation order、SPIR-V patch 支持集
docs/render-v2/F3_GPU_ORACLES.md           # G01–G09 真实 GPU 输入、期待输出与故障注入
# I 完成并独立资格后：
docs/render-v2/F3_VERIFICATION.md          # 仅 V 提交新增，其他 F3 文件必须在 I 中冻结
```

具体命名可沿仓库一致命名法调整，但职责与证据不能缺失。最终给用户的报告必须包含：

```text
Phase: F3 Shader/LayoutPlan/Native Pipeline
Approved base: c986e69765c9dd222469d4cf5090aa72549966dc
Implementation I: <full SHA>
Verification V: <full SHA>  (V only adds verification report)
Remote branch HEAD: <full SHA>
F3.0 Author Clear: PASS/PARTIAL/FAIL
F3.0 Shader Include: PASS/PARTIAL/FAIL
F3.1 LayoutPlan & Owner Full Shapes: PASS/PARTIAL/FAIL
F3.2 SPIR-V Relocation/Re-reflection: PASS/PARTIAL/FAIL
F3.3 Native Descriptor/Graphics/Compute RAII: PASS/PARTIAL/FAIL
F3.4 Real GPU G01–G09: per-test PASS/PARTIAL/NOT_RUN + device/caps
Tests: old 124 retained + new; regular/ASan two-build counts
GPU: device, enabled layers/features, validation errors, actual readback checksums
Perf: warm binding p50/p95/max/allocations; cold layout/PSO/relocation; GPU timing
Protection: frozen 719, R4 owners, Core/Transport, Final/history, six user edits
Evidence: commands, raw logs, SHAs, manifest SHA-256, clean clone identity
Not Run: Linux/Android/SDK/Plugin/Runtime/F4 etc exact list
F3 = PASS | PARTIAL | FAIL
NATIVE_RENDER_GRAPH = NOT_IMPLEMENTED (F4)
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT = F4 only after user review/authorization
STOP
```

**唯一最终授权结论：准许在当前入场基线开展 F3.0–F3.4 的完整 F3 实施；不设置独立 F3-PRE 阶段、不要求 F3.0 后再次人工批准；F3 完成后必须独立 I/V、推送、STOP。**
