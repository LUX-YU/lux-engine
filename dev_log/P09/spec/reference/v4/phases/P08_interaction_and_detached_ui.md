# P08 — 独立交互状态与底层 UI 离树构造/挂载协议

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P02, P03, P04, P06, P07。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M10 / M11 的 view_api；引擎 UI 基础。**关联问题：** A04、A05、T03、T06。**V3 回归：** Q02, Q10, Q26, Q29, Q34, Q44, Q45。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `SceneInteractionGroup / SceneSelection / InteractionGroupId` | `editor/tools/scene/interaction/include/lux/engine/editor/scene/SceneInteraction.hpp` | 交互服务拥有；视图仅关联 | 来源带作者/运行身份；共享选择不共享相机；不拥有 Session。 |
| `EditedObjectRef / RunningObjectRef / Gesture` | `editor/tools/scene/interaction/include/lux/engine/editor/scene/InteractionTypes.hpp` | 封闭来源值 / 私有手势状态 | 作者 UUID 与运行 Entity 不混；预览与已提交编辑分开。 |
| `MaterialInteraction / FlowInteraction` | `editor/tools/{material,flowforge}/interaction/` | 具体图交互组合 | 临时拖拽/选中/候选连接；最终调用具体域编辑，不共用万能图模型。 |
| `ViewId / ViewTypeId / ViewRestoreKey / ViewInfo` | `editor/views/api/include/lux/engine/editor/views/ViewInfo.hpp` | 视图身份和只读值 | ViewId 是 Host 代际身份；ViewRestoreKey 是持久键；ViewTypeId 直接复用 PaneTypeId。PaneId 留作 UI 树注册名，不与前两者互转。 |
| `DetachedView / IViewHost / ViewRequests` | `editor/views/api/include/lux/engine/editor/views/IViewHost.hpp` | 外层 CodeLease+unique_ptr<Pane>；窄 host 接口 | 工厂结果未挂 Root；请求 close/show/focus 不暴露 owning vector。 |
| `Pane/Element 的 detached 构造与内容拥有` | `modules/function/ui/include/lux/engine/ui/{Pane,Element}.hpp` | 保留真实 UI 继承，明确树 owner | Detached 状态无活动 Root 注册；节点是 owner 线程对象；非 worker 构造。 |
| `PreparedMountBatch / PreparedDetachBatch / AttachmentState` | `modules/function/ui/ 的 private UI 实现` | Root 局部准备/提交机制 | 先准备容量与路由，提交不调用可失败插件代码；Root 只登记不双重删除。 |

## C. 交互不等于作者状态

SceneInteractionGroup 维护选择、活动手势及其起始 ContentStamp。V1/V2 可共享一组选择，但各自相机/hover/输入 capture 留给视图；独立选择创建独立 group。group 不强拥有 Session，内容删除/重载后根据有效性通知清理失效目标。

手势采用 Begin→Preview→Commit/Cancel：Preview 写临时覆盖层，不进 SceneSnapshot、不改 dirty；Commit 将当前最终参数作为一次 SceneEditBatch；Cancel 只清理覆盖。期间源被其他视图修改，Commit 必须重新验证起始目标/戳，拒绝冲突或按具体字段策略处理，不能静默覆盖新状态。

Material/Flow 图的已保存节点布局由作者编辑提交，临时拖动路径和画布 pan/zoom 属于交互/视图。不能为抽出交互把原可保存的图坐标丢掉。

## D. UI 底层需要真实改动，不能只改工厂返回类型

现有 Pane 构造接收 Root/parent，Element 构造接收 Pane/Element。目标新增先离树构造的路径，Root 注册延后。只把返回值改成 unique_ptr、对象内部仍构造即注册，不通过本阶段。

### D1. 明确 UI 节点唯一 owner

采用显式拥有树：ViewHost 唯一持有顶层 Pane；Pane/具体容器用 unique_ptr 持有内容/子节点；Root 与 LuxObject 的活动路由/父子关系是非拥有关联。实施前查明 LuxObject 现有父子析构行为，确保它不会再删除这些同一对象；不得同时让 intrusive parent 与 unique_ptr 都承担 delete。

如果某个具体控件用值成员持有子 Element，它仍是唯一 C++ owner，Root 只记录观察关系；不得再把其地址包装成 owning unique_ptr。不强制所有小控件改成 heap 节点。关键是所有权表逐节点明确，而不是机械全改智能指针。

新增 detached 构造签名方向：`Pane(PaneId, PaneTypeId, title)`、`Element(ElementId)`；父子逻辑关联由容器在离树组装中设定。`attachedRoot()` 返回可空观察值，未挂载时焦点/路由请求返回 NotAttached；不能解引用空 root_。新代码禁止调用默认认为已挂载的 root()。

旧 rooted constructor 可作为唯一兼容路径暂留到 P12，必须登记 `[transition, deadline=P12]`，仅旧产品使用；新工厂/新视图的构造测试须证明 Root 的活动注册计数不变。不要在新 API 外面加一个同义 createDetached，再内部调用旧 rooted ctor。

### D2. 准备、接管、通知的顺序

1. 在 owner 线程构造完整 DetachedView，全部子对象/初始视图状态已准备，无 active Root/Session 修改。
2. Host 准备 owning 容量、Root 句柄槽、路由关系、布局挂载和通知容量。任何可恢复失败发生在这里，候选可安全析构，旧 UI 不动。
3. 安全修改阶段提交已经准备的关联与 owning 指针。此阶段不调 provider 的任意 `onCreate/restore`，不进行需要失败回滚的分配。
4. commit 后再发布通知；通知中请求的结构变更进入下一批次。通知投递失败不能把已经挂载的对象谎报成“从未发生”，使用独立诊断或准确部分报告。
5. 卸载先停新的输入路由、解除焦点/捕获与观察关联，再移除注册，等待当前回调栈退出，移交 GPU 退休责任，最后析构。

Root 与 Host 都不得在绘制/事件栈中同步销毁正在执行的节点。保留已有 deferChange/生命周期检查；新普通调用面用 ViewRequests 排队。`setFrozen(bool)` 不再作为外部随意切换全宿主的管理权限。

## E. view_api 的依赖与接口范围

view_api 可依赖底层 UI 值/Pane 协议以及 contracts，不能 include SceneSession、ProjectStorage、RenderRuntime 或 CommandRegistry 具体实现。ViewInfo 是 owning 快照，不公开 `span<const unique_ptr<Pane>>`。它可描述 UI type/title/visibility 与 opaque持久 restore key，不解析 Scene 内容。

已核对基准 `Ids.hpp`：PaneId/PaneTypeId 是 StableNameId，不是带 Host 代际的运行句柄。本版固定：`ViewId` 使用 Host 域＋slot＋generation，排队操作只携带它；`ViewRestoreKey` 使用独立 tag 的 StableNameId，只用于跨启动布局匹配；`ViewTypeId` 直接 alias 现有 PaneTypeId（同一类型身份，不另造 wrapper）。ViewHost 为每次挂载生成唯一低层 PaneId，并保存 ViewId→Pane 的映射；绝不从稳定布局 key 推导一个可被旧命令复用的 live handle。三个身份不同是因为有效域不同，不是多套可随意强转的同义数字。

这一具体编码选择是已有类型适配，不允许实施者同时保留“两种都支持”的永久分支。将选择和全部消费者变更写入一次 ADR/identity 映射，不影响 V3 三 owner 原则。

## F. 阶段出口与后续接口

本阶段提供可测试的 FakeViewHost 与真实底层离树/挂载实现，不做完整桌面 ViewHost（P10）。测试覆盖构造失败、Root 槽预算失败、当前回调请求 close、批次中再次 attach、未挂载析构、挂载后 detach 与回调/代码寿命。

需要新 private friend 时只允许 Host/Root 的确切协作入口；Scene/Material/Flow 任何 UI 类不得成为其 Session::Impl 的 friend。不得修改 LuxObject 的通用语义以迁就某一个工具而影响其他引擎对象生命周期。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| EditingGuard(bool&)；UI 中多份 editing_busy/busy/finishing 标志维持同一编辑门 | 新路径删除，使用唯一 Session EditGate 和私有 gesture state | SceneInteraction/MaterialInteraction/FlowInteraction；P08；旧工具壳 P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 新 Pane 工厂的构造即 Root 注册副作用 | 本阶段禁止；测试构造前后 Root 注册不变 | DetachedView + PreparedMountBatch；P08 | 已验证算法、资产兼容读取与行为回归不删 |
| Pane(Root&,...) / 旧依赖 Root 的 root() 使用 | 只给旧产品暂留并显式标期限；新 code 禁止 | detached ctor / attachedRoot / owner 挂载；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| Root/Pane/Element 的生命周期与 deferChange 检查 | 保留并适配，不以 shared_ptr 绕过 | UI 结构安全阶段；保留 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X08-01 / `gesture_preview_not_in_saved_snapshot` | Begin/Preview 时 capture，随后 Commit/Cancel | 预览不入 snapshot；提交恰一次历史；取消零历史变化 |
| X08-02 / `same_entity_number_different_source` | 作者/运行实体数值重合并复用对象槽 | 引用来源与代际校验，不能误编辑 |
| X08-03 / `detached_factory_has_no_root_effect` | 构造成功/失败/中途子节点失败 | 活动 UI 计数、焦点和注册完全不变，无双重 delete |
| X08-04 / `attach_prepare_failure_is_side_effect_free` | Host/Root 容量或通知准备失败 | 无半注册，候选销毁代码仍保活 |
| X08-05 / `close_inside_callback_is_deferred` | 在 active draw/signal 中请求关闭自身 | 回调完成前不析构，后续路由停止且最终释放一次 |
| X08-06 / `detached_access_is_explicit` | 未挂载时访问 focus/root/route | 有定义的 NotAttached/空观察值，不解引用无效 Root |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P08.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
