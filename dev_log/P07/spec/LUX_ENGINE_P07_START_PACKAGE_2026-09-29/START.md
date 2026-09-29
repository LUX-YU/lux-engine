# P06 R1 复审结论与 P07 启动指令

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**分支：`codex/editor-redesign-v4`**  
**本次复审验收 HEAD：`38fd05d551e4556f9d713b793de05f98db1bd156`**  
**P06 R1 实现截止：`2c8b189c61826355c6186be6c4c43eb06f8fafac`**  
**P06 前置验收：`cc3f455095f52e9a2740bf0a31a55e6672240916`**

## 0. 结论、证据边界与文件优先级

**P06 R1 在本次源码与归档证据审阅范围内通过。允许进入 P07，仅执行 P07；结束后等待复审，不自动进入 P08，不修改 main。**

本次核对了远端分支、验收父链、最终修正提交、Runtime 的实例结果拥有结构、发号/结算/回收/查询/确认路径、RunStore 查询确认、SceneStepStatus 的复制与代码保活、真实测试、部分修复前后日志、完整 CTest 汇总及归档检查器。[S1–S10]

没有独立执行引擎构建、126 项 Editor CTest、11 项 PLAYER 测试、42 项安装消费者、GPU 或 Android 测试；也没有执行整个 `check_receipt.py` 或逐份重算远端归档哈希。**本次不是新的运行验收，没有新增隔离探针成绩。** 附件和仓库报告中的运行数字属于实施方归档；阅读其测试和日志不等于独立重跑。

本文第 1–2 节是本轮复审。第 3 节以后是下一阶段指导，依据原 V4 `P07_projection_and_compilation.md` 及已验收事实展开，不表示这些 P07 类型已经存在。原 V4 作为历史参考保持不变，当前实现与当前施工账本优先于历史路径、种子清单和旧草图。

## 1. P06 R1 已经解决了什么

### 1.1 一个结果 owner，而不是运行时和 RunStore 各保存一份

`detail::InstanceLifetime` 现在保存唯一的 `std::array<Step, 32>` 和 `next_step`。原 Runtime `Record` 不再定义 Step/steps/next_step，只通过其 lifetime 使用这一结果表。发号、FIFO、实际执行、成功/失败/取消结算依旧由 SceneRuntime 完成。RunStore 的 ticket 列表只是它提交过的票据索引，不是第二个结果表。[S3]

`collectRetired()` 仍在停止维护完成后的安全点移除并析构重实例，然后设置退休完成。没有为了可查询结果，保留整个 Registry、SceneInstance、场景系统或 Renderer。

### 1.2 退休凭据延长的是结果寿命，不是重实例寿命

Runtime 的 `stepStatus` 和 `acknowledgeStep` 可以接受匹配的 `InstanceRetirement`。不提供凭据仍按活动实例查找；提供凭据则访问同一结果表，并保留 owner 线程、Runtime 域、实例完整身份和票据匹配检查。[S3–S4]

RunStore 使用自己的 retirement 调用这些接口。单项确认清掉一个终态槽；最终 `acknowledgeStop` 先确认该 Run 剩余结果，再删除 Run。外部持有 StopTicket/退休凭据的副本不会把已确认结果重新变成可查询。

已经复制出来的独立结果值可以继续存在；这与原票据查询入口失效是两件事。未确认的 STOPPED/FAILED Run 继续占用原 RunStore 容量，不建立无限历史墓碑。

### 1.3 错误结果不能比其代码寿命更长

`SceneStepStatus` 的代码 owner 声明在结果值之前，复制时一起保留，赋值使用持有旧值的交换方式。Runtime 在构建阶段收集相关代码 pin；真正失败的结果持 pin，重实例退休后释放创建期 pin。原 `DriveResult` 借用数组只保活到下一帧清空，不承担另一套票据调度。[S3–S4]

Runtime 查询时的错误复制、确认时的 payload 清理有 BusyScope；RunStore 的 stepStatus 也有外层 DispatchScope。自定义错误复制回调尝试确认票据或整个 Run 会得到 BUSY，不在复制途中删除被读记录。[S5–S7]

这不意味着任意拆出 `result` 字段或任意跨线程使用都已证明安全。本轮通过的是实际拥有型 SceneStepStatus 和规定的 owner 调用契约；不得将其宣传成任意插件 DLL 卸载认证。

### 1.4 真实负例与回归确实针对原组合

| 场景 | 已读取测试/日志验证的内容 |
|---|---|
| 排队后停止 | 实例槽位已经消失，Run 未确认，首次读到 CANCELLED/STOPPED，没有额外仿真步 |
| 完成一张再排两张 | 不提前读取票据；退休后依次为 COMPLETED、CANCELLED、CANCELLED |
| 实际发布错误 | 退休后首次读到 FAILED，保留 PUBLICATION 阶段和原 731 payload |
| 回调内停止 | 使用外部可控 pending 状态，外层 drive 仍 BUSY，真正结清后只析构一次 |
| 有界重复使用 | 64 轮槽位/代际复用、32 票据容量、单项/聚合确认、重复确认与旧身份拒绝 |
| 错误副本寿命 | 实例已毁、原槽已确认、借用 DriveResult 已结束，代码仍存活到最后结果副本清理 |

修复前 mixed 日志三项 `readable=0`，实例不存在且 Run 已停止；修复后三项均 `readable=1`，状态编号 2/4/4 分别对应 COMPLETED/CANCELLED/CANCELLED。其余两个修复前失败及正式总成绩在验收报告中记录。[S6–S9]

### 1.5 范围没有扩张成另一套运行框架

文件清单是十个源/配置/说明文件，集中于 Runtime、退休结果公开契约、Run 查询确认和原测试/SDK CMake。没有改变三作者模型、History、SessionState、保存链、目录或包名。旧私有冻结输入桥不扩权。[S10]

本轮没有发现必须继续阻止 P07 的前置问题。该判断不是全仓库无缺陷声明。

## 2. 下一阶段不得改写的交接事实

### 2.1 实际起点与工作区

从上述验收提交所在的实际实施分支继续。先检查工作区、HEAD 和祖先关系，不每阶段重置到最初调查提交，不强推。

附件记录 `editor/project/src/ProjectBuilder.cpp` 仍有既有用户修改，未计入资格实现。先核对并保护；不能把“独立验收检出干净”写成“用户工作区没有修改”。本轮未访问用户机器。[S9]

### 2.2 历史材料保持冻结

`.internal/editor-redesign/` 继续是唯一可变施工材料；P07 阶段末冻结到 `dev_log/P07/`。不要恢复原 V4 的另一套 `docs/editor-redesign/receipts/` 可变账本。历史源码验证按各自 implementation_sha，不改写旧报告、失败日志和哈希。

P06 的 Physics2D 生成依赖已有窄修复及缺失生成输入验证。保留原失败与修复记录；既不能继续无条件写“仍未修复”，也不能扩写成全仓首次冷构建通过。P06 R1 未重复修改该规则。[S9]

C01 仍归 P09/P12，C03 归 P11，C04 归 P12；原 FAIL 不改。P07 高亮原问题 C05 是本阶段责任，不能和这些有明确延期的原缺陷混为一谈。

## 3. P07 的目标和边界

**作者投影、视口资源、材质预览、Flow 编译/链接以及派生产物发布，成为具体、可独立验证的派生子系统。** 它们读取作者内容，不取代作者模型；使用 Runtime，不新增驱动宿主；发布产物，不冒充保存作者源。[P07]

目标关系：

```text
SessionStore → SceneSession / MaterialSession / FlowSession
       │                  │                  │
       │ snapshot/增量    │ snapshot         │ snapshot
       ▼                  ▼                  ▼
ScenePresentationHub   Material 编译记录    Flow 编译/链接记录
   → SceneProjection      → CompiledMaterial  → 固定派生产物
   → 多个 Viewport         → PreviewStore     → 发布操作
       │                  │                  │
       ├─ 原 SceneRuntime ┘                  │
       │  （唯一驱动与退休）                 │
       └─ 派生资源/提交回执     派生产物发布 ──┴─→ P05 同一个 WriteCoordinator
```

不得新增另一份可写作者世界、全局 Context、通用工作流引擎或跨域万能 Document。不要用 SceneSession 或 RunSession 继承关系把所有预览包装成运行；共享原有实例宿主不等于共享全部业务含义。

不实施 P08 手势交互框架、P09 布局、P10 完整新视图产品或 P12 总入口切换。P07 需要视口配置和 overlay 输入值，但不因此提前重建 UI。

## 4. 类型、组合关系与目录

下表是原 P07 的目标角色。优先复用已存在的等价类型，未找到再新增；不得制造同义别名或只有转发的空壳。

| 类型/角色 | 源码归属 | 关系及不得承担的责任 |
|---|---|---|
| ScenePresentationHub、SceneProjection、ProjectionVersion | `editor/tools/scene/projection/` | Hub 管共享派生记录；Projection 使用快照/受限增量，不拥有或改写 SceneSource |
| ViewportPresentation、ViewportConfiguration | 同一 projection 主题 | 每视口组合相机、尺寸、输出、overlay、在途提交；不复制整份作者世界 |
| HighlightRenderer、HighlightKey、PreparedHighlight、SubmitOutcome | projection 私有实现 | 分开 desired/prepared/accepted；只有提交成功才能推进 accepted |
| ResourceStatusSnapshot、ResourceRetryRequest | projection 的窄资源接口 | 表达派生资源事实；不把资源重试变成作者 EditBatch |
| MaterialCompileId、MaterialCompileOperation、MaterialCompileSettings | `editor/tools/material/preview/` | 固定快照、配置、环境和目标；TaskId 只关联执行进度 |
| MaterialPreviewStore、CompiledMaterial | 同一 material/preview 主题 | 持编译结果和预览资源；资源退休、预览采用与作者保存分开 |
| FlowCompileId、FlowCompilationService、FlowCompileOperation、LinkSettings | `editor/tools/flowforge/compilation/` | 编译和链接重试是具体过程；不持 FlowSession* 作为后台输入 |
| PublishCompiledMaterialOperation、PublishFlowArtifactOperation | 各自派生主题 | 复用同一个 P05 写协调器；不调用源码保存基线采用 |

原 P07 已给出对应公开头建议，参考包保留原表。小值放各自语义头即可，类型角色不等于一个目录、target 或 DLL。维持 `editor/editing/history/`、`editor/editing/sessions/`，不重新创建根目录 history/sessions。

## 5. P07 内部顺序（仍然是一个阶段）

### P07-A：盘点当前消费者与数据流

核对旧 SceneEditor 的投影/高亮/工作平面/提交代码，Material 的编译与预览，Flow 的编译/链接/发布。以当前代码和迁移账本为准，原 V4 的字段列表不是要求重新创建已删除字段。

每条路径记录：输入身份与版本、准入点、实际工作 owner、完成接收、采用判据、重资源退休、结果确认、容量与最后消费者。明确哪些旧纯算法会迁出，哪些仅是旧窗口输入转换。

尤其检查已有后端高亮寻址是 scene/feature 级还是 viewport 级。不要先把 CPU 类拆完，再发现两个视口仍写同一后端状态。

### P07-B：先做共享作者投影及版本恢复

使用 SceneSnapshot 或 owner 内受限增量更新。共享键必须区分会话/有效历史与投影配置；原 Entity 不是作者身份。增量裁剪/换代时消费 RESET_REQUIRED，而非把缺少的增量视为“没有变化”。

多个视口使用同一份共享派生场景/mesh 数据，独立输出和 view-local 状态。关闭最后使用者后按确定的有界缓存策略释放或进入退休，不无限保留访问过的投影。

实例创建与退休复用 P06。Hub、Viewport、PreviewStore 不能各自再 driveFrame。需要组装包时，先复核 P06 已提取的 `buildSceneSnapshotPackage`；不要重新复制一份保存/运行共有的分区组装算法，也不要 encode 后立即 decode 作为惯常中转。

### P07-C：实现视口隔离和完整高亮提交协议

| 阶段 | 必须保持的事实 |
|---|---|
| desired | 当前需要的投影/选择/feature/视口版本，不表示资源已经准备好 |
| prepared | 候选 program 与完整 pins 已准备，尚未被后端接收 |
| accepted | 后端明确接受；不等于 GPU 已结束 |
| retired/completed | 实际最后使用已经结束，资源可以释放；结果观察与重资源寿命分开 |

捕获临时失败时不能更新 accepted，相同 key 下次仍要真正重试。Backpressured 保留完整候选和 pins；新 desired 替换尚未提交旧候选时安全释放旧输入，不能将旧结果冒充新版本。永久拒绝记录诊断，按依赖变更或显式 retry 恢复，不能每帧无条件刷错。

view-local 不只是 CPU key 多一个 ViewId。原后端若仅支持场景全局高亮，应只在对应 feature/overlay 路径补齐真实视口绑定，不能靠复制整个场景回避。保留原渲染算法，不借此重写 RenderRuntime/Vulkan。

### P07-D：Material 编译与预览

固定 MaterialSnapshot、编译配置、编译环境版本与预览目标代际；worker 只持拥有输入。采用时复查全部关键条件，不只比较 TaskId。

S10 晚到不能覆盖 S12。S12 失败时可以继续显示最后可用 S10，但必须明确是旧效果并保持其真实 stamp；不能将它标成当前编译成功。预览关闭/代际复用后到达的结果只正确结清，不重新激活旧目标。

编译完成、CPU 结果可用、GPU 资源被接受、实际渲染完成应准确区分。沿用 P05 R2 的可靠完成接收和 P06 R1 的结果寿命原则，但不要为了复用原则再引入一个所有服务共用的结果管理器。

### P07-E：Flow 编译、链接与固定产物重试

复用现有 FlowCompilation、工具链、ScriptArtifact ABI 和环境寿命。编译输入为拥有型 FlowSnapshot；不得边编译边读取 live 图。

`retryLink(compile_id, settings)` 使用该编译记录的原输出和来源。随后作者图改到新版本，不改变旧链接重试的输入。产物因容量策略被释放后，明确报告不可重试或需要重新编译，不能默默抓取最新图。

缺 linker 是该工具链操作的结构化失败，不隐藏或禁用已完成的 Flow 作者模型。P07 必测的真实编译/链接未运行，则相应门槛不能写 PASS；不把这一条解释为整个作者域倒退。

### P07-F：派生产物发布、旧调用收拢与最终验收

Material 和 Flow 派生产物发布都借用 P05 同一个 WriteCoordinator，遵守规范目标、来源、FIFO、冲突和 Unknown 责任。不能另建按窗口/模型的写队列，也不能为通过结果排序而无条件刷新当前文件版本。

产物发布不是源码保存：成功不调用作者 checkpoint 的 accept/rebind，不将 dirty 清零。源保存和产物发布若故意使用同一物理目标，必须遵循现有来源/格式冲突规则，不能因复用 SessionId 就误认为二者字节语义可互换。

新生产路径使用共享纯算法后，应迁移原调用、删除原纯算法体；旧窗口壳只保留必要的输入/显示转换，并有消费者与期限。最后整体运行 P07 与原回归，冻结准确实现 SHA。

## 6. 继承本轮结果寿命经验，但不复制 32 项数组

P06 的 32 项容量是单步协议，不是所有编译/预览服务的默认魔数。P07 应根据各自已有容量设计确定可验证上界。

明确区分：

- **重资源**：GPU buffer/image、实例、编译器内部对象、加载模块；按实际最后使用退休。
- **操作结果**：编译诊断、提交拒绝、发布结果、退休完成；按公开确认/保留策略提供查询。
- **可重试输入**：Flow 原编译产物、尚未接受的高亮候选；只在明确预算及责任内保留。

资源已退不代表诊断已确认；结果可读也不意味着必须保活全部源模型或 GPU。错误值及回调的复制/析构可能执行扩展代码，应在最后使用前保留必要 code/data owner，并维持适当执行期保护。

合法已准入工作的完成不能 BUSY 后被丢弃；允许接收完成不能顺带开放递归采用和执行期删除。只拒绝新的业务准入，不重跑已经完成的工作来掩盖结果丢失。

## 7. 明确的删除与暂留要求

| 原角色/符号（按当前账本复核） | P07 动作 | 可暂留与禁止 |
|---|---|---|
| 提前写入 highlighted_selection/structure/feature/instance、highlight_pending 的新路径 | 由明确的 key 与 desired/prepared/accepted 取代 | 不把旧 bool/字段原样移入新 Impl 继续双写真相 |
| SceneEditor::Impl::updateHighlight/updateWorkPlane/renderFor/submit | 提取唯一派生算法并使原消费者调用 | 旧窗口输入/显示壳可按 P12 暂留；不能复制算法后原体不删 |
| Material Compilation 的 history/state/revision/preview_assets/status/output、compile_task_/compile_result_ | 移到实际编译记录和 PreviewStore | 旧 UI 只读窄结果，不继续拥有编译/资源状态机 |
| Flow Compilation/acceptCompilation/retryLink/requestPublish | 迁入具体编译/链接/发布服务 | 工具链 ABI 与算法保留，不另造脚本运行层 |
| 派生发布中对作者保存基线的更新 | 新路径禁止并测试 | 不因旧 API 方便而调用源码 SaveSource::accept |
| 新 Runtime 驱动/回收旁路 | 不得引入 | 所有派生实例都沿 P06 唯一宿主和退休协议 |

同步声明、定义、调用、friend、生成输入、CMake/安装导出、活动测试及示例。不允许 `.old`、大段注释、`#if 0`、空转发别名充当删除。用户数据、只读旧格式解析和负例属于独立保留对象，不应误删。

## 8. P07 原七项场景及真实验证边界

| 原编号 | 实际出口要求 |
|---|---|
| X07-01 | 同 key 首次捕获失败，下一次资源可用后真正重试；accepted 不提前变化 |
| X07-02 | prepared→背压→accepted→视口关闭→实际最后使用；每一步 pins/资源存活正确 |
| X07-03 | 共享场景、不同视口、不同选择；验证真实后端绑定路径，不只比较 CPU key |
| X07-04 | Material 编译乱序、较新失败、环境/配置/目标变更；陈旧标识与采用准确 |
| X07-05 | Flow 编译后改图，再 retryLink 旧产物；来源不变、不隐式重新编译 |
| X07-06 | 未保存源码编译并发布实际产物；文件结果正确，作者 dirty/checkpoint 不被改变 |
| X07-07 | 增量裁剪后重建、关闭最后使用者、反复创建/释放；缓存有界，不按视口复制作者世界 |

原 P07 F2 将完整新产品真实 GPU 视口隔离与布局/资源退休资格安排在 P10/P13；本阶段不提前实现两套新窗口产品。P07 仍须实际运行 CPU 编译、可用后端预览/资源路径，并验证后端寻址确已隔离。Fake 可以验证故障顺序和 pin，但不能只造两个 mock map 就声称 X07-03 的真实绑定已经完成。缺后续产品集成是明确后续责任，不是本阶段可以留空后端路径的理由。

原 126 项 Editor、11 项 PLAYER 和十组 SDK 消费者的相关断言继续保留。当前 42 项安装成绩是前阶段基线，不是要求下一阶段测试数量永远不变。API 迁移导致测试改动时逐项解释等价行为，不删除检查来保住计数。

执行实际依赖负例：作者模型不反向链接投影/Runtime/编译器；新派生服务不依赖旧 EditorContext/大 Impl；Engine 不依赖 Editor；派生发布不绕过既定写协调器。负例必须失败在预期规则，同夹具修复后通过。

最终显式 `LUX_EDITOR_MIGRATION_STAGE=P07`，不能使用 P06 门禁报告 P07 完成。

## 9. 交付和可直接发送的指令

收据至少记录三个派生子系统的输入/版本/结果/资源 owner、后端视口身份、编译与链接重试规则、写协调器实例来源、确认与容量、迁出/删除/暂留清单，以及七项 X07 与实际工具链/后端/安装矩阵。

> P06 R1 复审通过，允许进入 P07，仅执行 P07。以 38fd05d551e4556f9d713b793de05f98db1bd156 为前置，核对分支与工作区，保护 ProjectBuilder.cpp 既有修改，不 reset、不改 main。
>
> 读取原 P07、本启动补充及当前施工账本。按 A–F 内部顺序实现共享作者投影、视口本地高亮/资源、Material 编译预览、Flow 固定产物链接重试及派生产物发布。保持原 Runtime 唯一驱动，所有派生实例沿现有 lease/retirement。
>
> 使用拥有快照与明确内容/配置/环境/目标版本，陈旧完成只结清不冒充当前效果。分开 desired/prepared/accepted/实际退休；背压不丢 pin，相同 key 捕获失败能够重试，后端高亮确实按视口隔离。
>
> 产物发布借用 P05 同一个 WriteCoordinator，不修改作者 checkpoint。继承已准入完成可靠接收和重资源/可查询结果分离，不新增通用管理器、第二套 Runtime、结果队列或源码副本来替代清楚的责任。
>
> 迁出算法后删原体，旧窗口只保留有消费者和期限的转换壳。保持 scene/projection、material/preview、flowforge/compilation 的主题归组，不为每个小类型增加库，不恢复根 history/sessions。
>
> 执行 X07-01～07、相关 Q、原模型/保存/Run 与 R1/R2 回归、实际编译/文件 IO/后端依赖负例、PLAYER 和 SDK，显式 P07 门禁。C01/C03/C04 保持原 FAIL/责任；P07 自己的问题不得借旧编号延期。实现与 dev_log/P07 验收分开提交，正常推送后停在 P07，不进入 P08。

## 10. 固定来源

- [S1 分支本次验收提交](https://github.com/LUX-YU/lux-engine/commit/38fd05d551e4556f9d713b793de05f98db1bd156)
- [S2 最终实现修正](https://github.com/LUX-YU/lux-engine/commit/2c8b189c61826355c6186be6c4c43eb06f8fafac)
- [S3 SceneRuntime.cpp](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/engine/scene/composition/src/SceneRuntime.cpp)
- [S4 SceneRuntime.hpp](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp)
- [S5 RunStore.cpp](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/editor/tools/scene/execution/src/RunStore.cpp)
- [S6 runs.cpp](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/editor/tools/scene/execution/test/runs.cpp)
- [S7 runtime.cpp](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/engine/scene/composition/test/runtime.cpp)
- [S8 修复前 mixed](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/before/r1-mixed.log) / [修复后 mixed](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/logs/run-r1-mixed.log)
- [S9 P06 R1 验收报告](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/README.md)
- [S10 文件清单](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/files.json) / [检查器](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/check_receipt.py) / [CTest](https://github.com/LUX-YU/lux-engine/blob/38fd05d551e4556f9d713b793de05f98db1bd156/dev_log/P06-R1/logs/ctest.log)
- [P07 原 V4 实施文档（在启动包内）](reference/v4/phases/P07_projection_and_compilation.md)
