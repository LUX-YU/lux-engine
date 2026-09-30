# 回归与阶段门槛矩阵

V3 的 Q01–Q52 沿用，另加本实施包 X 场景。所有记录初始都是 NOT_RUN。阶段文档中“相关 Q”表示本阶段需要实现/检查相关部分；下表的“首次完整门槛”规定第一次必须完成的整体层次。较早阶段不必为尚未进入范围的 UI/GPU 创建假实现，但必须执行本阶段 X 场景；也不能把局部 fake 通过写成完整 Q 已通过。P13 复验全部适用场景与实际平台矩阵。

P00 对 Q31/Q38/Q42 是建立缺陷证据与修正预期，不要求在基线阶段先完成后续改造。Q45/Q49 的现有图检查与测试分组门禁从 P00 生效，之后每新增 target 都重新执行；Q46 在 P11 扩展安装切片完成，P13 扩为最终全 SDK。

Q25 的适用性只能根据 P00 的既有功能盘点冻结，不能临近终局把一个已经可用的功能改成 N/A。未提供新 ApplyRunChanges 时，必须实际验证 Stop 不隐式回写，界面不得假装此能力已实现。

## 1. V3 功能回归

| ID / 名称 | 首次完整门槛 | 环境 | 操作 | 结果 |
| --- | --- | --- | --- | --- |
| Q01 领域无桌面启动 | P02 | native/build | 不创建 Root、Pane、GPU 或 ProjectStorage，建立 Scene/MaterialSession 并编辑/捕获 | 领域行为可用；domain consumer 的链接与 include 闭包无 UI/渲染实现 |
| Q02 双视图单一作者态 | P10 | native/desktop | V1/V2 绑定同一 Session，在 V1 编辑并从 V2 undo | 一次编辑只有一个历史条目；两视图观察同一状态，相机仍独立 |
| Q03 关闭一个视图 | P10 | native/desktop | 关闭两个视图中的一个 | 会话、历史及另一视图保持；只退休该视图资源 |
| Q04 布局不替换作者内容 | P12 | native/desktop | 有未保存场景时应用含同类窗口的布局 | 内容/HistoryId/dirty 不被布局恢复覆盖；视图只重新排列或绑定 |
| Q05 失败和过期读取 | P12 | fake IO | 读入失败、乱序完成、完成前重载或更换目标 | 原工作副本保留；旧结果不能采用；终态说明拒绝原因 |
| Q06 身份代际与类型校验 | P01 | native | 释放后复用槽位；使用错类型 key、旧 HistoryId 或旧运行对象 | 返回失效/类型错误；不访问新对象，不靠同一整数推断相同身份 |
| Q07 撤销回保存点 | P01 | native | 保存 S10、编辑 S11、undo 到 S10 | 本绑定基线仍 S10；当前恢复 clean；通知版本可继续增长 |
| Q08 字节相同不伪造状态等价 | P02 | native | 重新执行编辑得到相同字节但新 StateId | 按文档保守定义仍 dirty；不能混用内容 hash/通知版本清除标记 |
| Q09 编辑准备故障 | P02 | fault injection | 在 prepare 校验、预算或预备资源阶段注入失败 | 作者内容、历史游标和可观察选择不发生半提交 |
| Q10 编辑门和许可语义 | P08 | compile/native | 编译期复制 EditScope；嵌套进入编辑；移动/释放 ClosePermit | EditScope 不可复制/移动；重入被拒；permit 未消费只解除限制 |
| Q11 冻结快照与后台寿命 | P05 | native/sanitizers | 捕获后继续编辑/关闭视图，延迟编码和完成采用 | 编码对应捕获状态；无 live Session/Pane 裸指针跨任务；所有输出显式拥有 |
| Q12 保存旧状态时继续编辑 | P05 | fake IO | 捕获 S10 后编辑到 S12，再完成保存 | 基线 S10、当前 S12，仍 dirty；不标记回调时的当前状态已保存 |
| Q13 反序编码、有序发布 | P05 | fake IO | W2 编码先完成；W1 延迟/失败/取消；最后再保存撤销后的旧 StateId | 同目标按准入发布；空洞可终结；最后意图决定最终文件，不按 StateId.serial 排序 |
| Q14 两个副本同目标冲突 | P05 | fake/real IO | 两 Session 基于同存储版本独立保存到同一规范化地址 | 后一写入按后端契约报告冲突，不能静默串接成同一工作副本的后继 |
| Q15 Save As 发布失败 | P05 | fake IO | 重绑定候选已备妥，在新文件发布前注入失败 | 源地址、BindingRevision、历史和保存基线不变，无部分身份采用 |
| Q16 Save As 冻结与普通 Save | P05 | native | 分别在正常 Save 和 Save As 飞行期间编辑、重载及再次保存 | 正常 Save 允许编辑；Save As 按 permit 拒绝冲突操作，结束准确释放 |
| Q17 提交后取消与记录清理 | P05 | fake IO | 后端已发布，随后 cancel 或 acknowledge | 结果为已提交/取消过晚；acknowledge 不撤销文件、不清除未完成资源责任 |
| Q18 关闭与迟到保存结果 | P12 | native/sanitizers | 处理关闭期间保存；删除 Session 后注入已允许保留任务的完成 | 无 UAF；已发布事实保留，不能采用到重用槽位；新绑定不被旧回执污染 |
| Q19 全体关闭后项取消 | P12 | native | A 已保存，B 选择取消；或第二个 permit 失败 | 任何 Session 均未被提前销毁；A 的真实保存保留并报告；permit 全部释放 |
| Q20 审阅戳过期 | P12 | native | 对 S10 显示丢弃确认，答复前内容变成 S12 | 不能丢弃未经审阅的 S12；重新审阅或明确拒绝 |
| Q21 最后视图策略 | P12 | native/desktop | 关闭最后视图，分别选择保留内容、关闭内容、取消 | 三种行为可区分；取消不先销毁最后可用视图 |
| Q22 运行隔离 | P06 | native/runtime | 从 S10 启动 R；编辑作者源到 S12；运行中改变对象 | 作者和 R 不互相改写；provenance 保留 S10；无双可写事实 |
| Q23 暂停仍维护 | P10 | runtime/GPU | 暂停仿真后完成资源加载/同步/发布 | 仿真时间不前进，必要维护和呈现可前进；valid 不再被误用作存在性 |
| Q24 步进回执 | P06 | native/runtime | 连续 Step、旧 RunId 的 Step 和执行失败 | 票据仅在对应真实仿真步完成后成功；旧运行和失败不假完成 |
| Q25 显式应用运行修改 | P12 | native | 运行基线后作者发生冲突修改，再请求 ApplyRunChanges | 冲突按策略拒绝/审阅；成功只通过一次作者编辑并可 undo；stop 不回写 |
| Q26 选择身份隔离 | P08 | native | 作者/运行对象数值相同；对象删除/重载/代际复用 | 不会误选另一来源对象；失效选择清理，不重复删除领域内容 |
| Q27 材质编译迟到 | P07 | fake compiler | S10 编译晚于 S12 完成；或较新编译失败 | 旧结果不能覆盖新配置/内容；合法作者编辑和历史不因编译失败回滚 |
| Q28 高亮捕获失败重试 | P07 | fake render | 选择保持不变时资源捕获首次失败、第二次成功 | accepted key 未前移；下一轮仍尝试或消费保留候选 |
| Q29 背压与资源退休 | P10 | fake render/GPU | 后端暂拒提交；随后接受但 GPU 尚未完成；关闭视图 | desired/prepared/accepted 分明；资源和代码 pin 留到真实最后使用点 |
| Q30 投影漏事件恢复 | P07 | native | 裁剪增量队列或重载导致 changesSince 不可提供 | 返回 ResetRequired 并重建；不永久卡在旧投影，不读取过时借用 |
| Q31 坏 docking 数据 | P12 | native/desktop | 合法部分 Pane 状态配损坏 dock 记录 | 纯验证失败前不创建活动窗口、不改可见性；作者内容不变 |
| Q32 缺失扩展载荷 | P09 | native | 布局含未安装 provider 和未知 schema 扩展字段 | 报告不可用；载荷原样/按协议保留，保存往返不静默丢失 |
| Q33 额外视图和未保存内容 | P12 | native/desktop | 布局不包含当前额外窗口且内容 dirty | 默认保留额外窗口和工作副本；显式报告处理策略 |
| Q34 工厂/挂载准备失败 | P10 | fault injection | DetachedView 构造、状态准备、Host 容量准备失败 | 无幽灵 Pane 或半注册窗口；已有布局保持；不得依赖任意插件外部副作用可回滚 |
| Q35 稳定布局身份重命名 | P09 | fake/real IO | 重命名显示标签，同时令 preferences 不可写 | 布局按稳定 LayoutId 保持可选；重命名不需要跨文件更新活动引用 |
| Q36 布局成功、偏好失败 | P12 | fake IO | 布局已保存或采用，再令偏好保存/目录刷新失败 | 结果区分两件事；不得把已提交布局报告成未发生 |
| Q37 删除活动布局回退 | P12 | native/desktop | 删除当前布局后重启，偏好仍引用缺失 Id | 按明确默认回退并报告，不因旧偏好指针使启动崩溃 |
| Q38 QUERY 自替换注册 | P11 | native/sanitizers | 查询回调请求替换包含自身的注册表并在随后访问捕获状态 | 旧 entry/code 存活到调用返回；发布延迟/拒绝，不边销毁边执行 |
| Q39 恢复批次注册变化 | P12 | native | 恢复第一个视图时扩展集合请求变更 | 整个批次使用固定 snapshot；不出现迭代失效与半批次不同版本 |
| Q40 准入不是完成 | P12 | native | 命令被接受，实际保存/打开随后失败或目标消失 | DispatchReceipt 表示准入，最终 outcome 表示失败；界面不提前显示完成 |
| Q41 焦点改变不漂移目标 | P11 | native | 提交后切换焦点、关闭旧目标并复用槽位 | 操作仍针对原目标或拒绝失效，不自动作用于当前焦点的新内容 |
| Q42 菜单初始化失败 | P12 | fault injection | 注入菜单信号/注册初始化失败 | EditorApplication 创建返回失败；不得发布已标记退出的成功对象 |
| Q43 有序关闭与代码保活 | P12 | integration/sanitizers/GPU | 飞行中编码、GPU 提交、插件回调同时遇到退出 | 停止准入→排空/终态→退休→析构→卸载；不依靠 UI 泵已停止后等待 |
| Q44 部分初始化与 OOM 契约 | P12 | fault injection | 各初始化点失败和受测分配点耗尽 | 按声明策略清理；声称可恢复的 API 不偷偷 terminate；终止策略不伪称可恢复 |
| Q45 负向依赖检查 | P00 | build | 人为让 scene_model include Pane、workflows 链接 bootstrap 或 engine 依赖 editor | 配置/CI 明确失败；测试失败不能被 PUBLIC/PRIVATE 标签掩盖 |
| Q46 独立安装消费者 | P11 | installed/build | 仅安装前缀在干净构建树编译插件与工具消费者 | 不读取源码私有头、测试后门或原 build tree；所需依赖被正确导出 |
| Q47 Player 与通用运行层独立 | P06 | build | 禁用所有 editor target 配置/编译 Player 和 runtime consumer | 不需要编辑器头/库/插件表；运行职责未搬到 editor |
| Q48 SDK 闭包独立 | P11 | build/installed | 只修改编辑器扩展契约，检查运行插件构建契约 | 运行插件不无故依赖编辑器 SDK 指纹；兼容拒绝有准确范围 |
| Q49 普通测试跨平台配置 | P00 | build/CI | 无 PowerShell 和本机定制 LLVM 路径环境配置 native tests | 普通测试可用；平台/GPU 组明确选择与缺失原因，不阻断领域测试 |
| Q50 性能和容量测量 | P13 | benchmark | 单/双/多视图、保存快照、投影积压、持续保存 | 记录时间/峰值内存/复制/分配/退队情况；无无界保留，无未测量提速结论 |
| Q51 坏加载/工厂无幽灵对象 | P12 | native/desktop | 准备失败或内容已发布但视图失败 | 发布前无半会话；发布后明确保留无视图内容并报告；无幽灵 Pane |
| Q52 SDK 与载荷版本拒绝 | P11 | installed/native | 加载不匹配 SDK 契约或不支持 schema 的状态 | 在调用不兼容入口前拒绝；未知载荷保留；不把指纹相等当作无限 ABI 保证 |

## 2. 本实施包新增门槛

| 阶段 / ID / 名称 | 操作 | 结果 |
| --- | --- | --- |
| P00 / X00-01 / boundary_negative_fixture | 故意让 scene_model 样例 include Pane | 检查器因禁止的依赖失败；移除错误边后同环境通过 |
| P00 / X00-02 / native_tests_without_platform_linker | 无 PowerShell/lld-link 的 native-only 配置 | 领域测试可配置；真实 linker 测试显式未选择，不是整个工具功能被删 |
| P00 / X00-03 / legacy_inventory_is_total | 遍历四个 Impl、Context、PaneManager 的声明与引用 | 每项都有处置与期限；未知项不为零则不能进入 P01 |
| P00 / X00-04 / baseline_failures_are_not_successes | 读取失败日志和既有测试清单 | 原失败保留；测试迁移覆盖未因删测试而缩减 |
| P01 / X01-01 / history_has_no_persistence_members | 编译公开历史头并扫描 API/成员 | 不存在 beginSave/finishSave/saved/clean；原编辑 prepare/commit 测试仍通过 |
| P01 / X01-02 / session_slot_generation_and_type | 删除后复用槽位，并尝试错类型 key | 旧 key/type 失败且不返回新对象 |
| P01 / X01-03 / checkpoint_after_undo_and_reload | 建立保存点后 edit/undo，再整体换 HistoryId | 撤销回保存点 clean；旧世代回执拒绝 |
| P01 / X01-04 / scope_and_permit_lifetime | 编译复制/移动 EditScope 的负例；移动及释放 permit | scope 非复制移动；permit 释放恰一次；无错代际解锁 |
| P01 / X01-05 / legacy_bridge_uses_single_checkpoint | 旧工具保存失败/成功/候选失败各一次 | History 不保存 saved；桥与内容切换一致，不复制第二份基线 |
| P01 / X01-06 / session_candidate_failure_no_publish | 候选、代码 lease 或槽位预算故障 | 无可见半会话；析构在代码卸载前发生 |
| P02 / X02-01 / scene_session_headless_real_content | 不创建 Root/Runtime/GPU，用真实 CPU 场景建立会话 | 结构/字段编辑、undo、capture 可用，公开依赖闭包干净 |
| P02 / X02-02 / scene_edit_batch_is_atomic | 多对象插入时第三处预算失败；重挂父形成环 | 所有作者对象和历史游标保持；没有半提交 |
| P02 / X02-03 / scene_snapshot_freezes_plugin_payload | 捕获后继续修改节点并延迟释放插件加载句柄 | 冻结内容不变，deleter 执行前代码仍存活 |
| P02 / X02-04 / author_identity_survives_storage_rebind_design | 不同打包根地址使用相同作者 UUID 场景样本 | 领域寻址不依赖运行 Entity 或当前持久 AssetId |
| P02 / X02-05 / changes_since_overflow_requires_reset | 裁剪增量记录并整体换 HistoryId | 明确 ResetRequired，不返回伪完整增量 |
| P03 / X03-01 / material_graph_without_preview | 无 GPU/窗口/编译器创建材质并操作节点/连接/参数槽 | 编辑与 undo/redo/capture 可用 |
| P03 / X03-02 / material_owned_node_transfer_failure | 插入/替换节点准备阶段失败 | 节点无泄漏/双删；原图和 history 未变 |
| P03 / X03-03 / material_graph_positions_are_authored | 移动节点并保存快照，另改视图 pan/zoom | 持久布局进入快照；视图变换不污染 source |
| P03 / X03-04 / material_snapshot_and_plugin_lifetime | 快照后替换动态节点，晚析构旧快照 | 冻结值不变，插件代码覆盖最后析构 |
| P04 / X04-01 / flow_graph_without_linker | 环境中无 linker，编辑变量/函数/导出并 undo | native 模型全部可用；没有通过禁用工具绕过 |
| P04 / X04-02 / flow_referenced_variable_and_signature_failure | 删除仍被引用变量；修改签名造成无效连接 | 按已确定领域策略拒绝/统一修改，失败没有半图 |
| P04 / X04-03 / flow_environment_outlives_graph_and_history | 释放加载器外部句柄后销毁快照/会话 | 节点与 memento 析构仍有有效代码 |
| P04 / X04-04 / three_sessions_share_only_real_contracts | 三种 Session 同 Store 查询、关闭准备与类型校验 | 无 Scene 特例、无 Unsupported 虚方法大基类 |
| P05 / X05-01 / reverse_encoding_keeps_publish_order | W1 慢/W2 快，随后 undo 到更旧状态再 Save | 磁盘最终对应最后准入意图；不按 StateId 序号排序 |
| P05 / X05-02 / failed_or_cancelled_ticket_unblocks_lane | W1 编码失败或确认取消，W2 已 ready | 队列前进，无 worker 同步等待导致饥饿 |
| P05 / X05-03 / unknown_publish_does_not_race_successor | W1 返回不确定且可能晚发布，W2 就绪 | 不允许 W2 被晚 W1 覆盖；lane 仅在责任结清后继续 |
| P05 / X05-04 / old_saved_content_does_not_clean_current | 捕获 S10 后编辑 S12，倒序投递回执 | 基线准确，无回退；新历史/绑定不被污染 |
| P05 / X05-05 / save_as_keeps_history_and_origin_on_failure | 新目标发布前失败、发布成功、发布不确定分别运行 | 失败原绑定/历史完整；成功保留 undo；不确定无自动错误采用 |
| P05 / X05-06 / two_sessions_same_target_conflict | 两副本以同基线经不同别名保存 | 同 lane，第二副本冲突不能静默继承前序版本 |
| P05 / X05-07 / late_completion_after_session_removed | 注销角色、删除 Session、复用槽后投递完成 | 仍记录磁盘事实，无 UAF/新对象污染 |
| P05 / X05-08 / real_file_commit_reports_actual_fact | 真实临时文件替换后使后续持久性/偏好步骤失败 | 报告已发布与后续失败，而不是未发生；原始证据保留 |
| P05 / X05-09 / save_result_retention_is_bounded | 持续保存直到容量阈值并 ack/cancel | 准入背压正确，无无限增长、提前释放或漏终态 |
| P06 / X06-01 / one_runtime_driver_per_instance | 同 frame 同时有作者投影、运行与两个视图消费者 | 实例实际仿真/维护推进不重复，无 editor 私建 Runtime |
| P06 / X06-02 / pause_maintains_without_time_jump | 暂停时注入资源完成，再恢复 | 维护前进，仿真时间停住，恢复不补巨大 dt |
| P06 / X06-03 / step_tickets_track_actual_steps | 连续 step、执行失败、旧 RunId 与队列满 | 每张成功票据对应真实一步；错误不假完成 |
| P06 / X06-04 / retire_in_callback_is_deferred | 回调中请求停止/退休，存在飞行资源 | 不立即 destroy 当前对象、不阻塞/terminate；安全点最终释放一次 |
| P06 / X06-05 / run_snapshot_is_not_author_source | S10 启动后作者改到 S12，运行又改对象 | 两份语义隔离，stop 不回写，provenance 为 S10 |
| P06 / X06-06 / player_build_without_editor | PLAYER 配置完全不包含 editor targets | 引擎运行场景仍可用，未产生反向 Editor 依赖 |
| P07 / X07-01 / highlight_capture_failure_retries_same_key | 相同选择首次捕获失败，下一次可用 | accepted 不变且下一次真正重试 |
| P07 / X07-02 / backpressure_pins_survive_view_retirement | 准备→背压→接受→关闭→GPU 结束 | 每步资源/代码 lease 在最后使用前不释放 |
| P07 / X07-03 / two_view_overlay_state_is_not_scene_global | 共享场景两个不同视口提交不同选择 | 实际后端绑定分离；不得只比较 CPU cache key |
| P07 / X07-04 / material_out_of_order_compile_and_failure | S10 晚、S12 早，另测 S12 失败 | 采用与陈旧标识准确；作者内容/dirty 不变 |
| P07 / X07-05 / flow_retry_link_uses_original_artifact | 编译后修改 graph，再 retryLink 旧结果 | 固定旧编译 stamp；不隐式重编译当前源 |
| P07 / X07-06 / compiled_publish_does_not_clean_source | 未保存源编译并发布产物 | 文件发布准确但作者 dirty 仍保持 |
| P07 / X07-07 / projection_reset_and_capacity | 裁剪增量并关闭最后使用者，持续创建/释放投影 | 可重建，有界缓存，无多视图复制作者世界 |
| P08 / X08-01 / gesture_preview_not_in_saved_snapshot | Begin/Preview 时 capture，随后 Commit/Cancel | 预览不入 snapshot；提交恰一次历史；取消零历史变化 |
| P08 / X08-02 / same_entity_number_different_source | 作者/运行实体数值重合并复用对象槽 | 引用来源与代际校验，不能误编辑 |
| P08 / X08-03 / detached_factory_has_no_root_effect | 构造成功/失败/中途子节点失败 | 活动 UI 计数、焦点和注册完全不变，无双重 delete |
| P08 / X08-04 / attach_prepare_failure_is_side_effect_free | Host/Root 容量或通知准备失败 | 无半注册，候选销毁代码仍保活 |
| P08 / X08-05 / close_inside_callback_is_deferred | 在 active draw/signal 中请求关闭自身 | 回调完成前不析构，后续路由停止且最终释放一次 |
| P08 / X08-06 / detached_access_is_explicit | 未挂载时访问 focus/root/route | 有定义的 NotAttached/空观察值，不解引用无效 Root |
| P09 / X09-01 / invalid_dock_is_rejected_before_plan_effects | 完整视图条目配坏 dock/重复 ID/环 | 纯验证失败，任何 UI/Session 计数不变 |
| P09 / X09-02 / rename_layout_with_unwritable_preferences | 修改 label 成功，prefs 权限拒绝 | 同 LayoutId 仍可选择，报告独立结果 |
| P09 / X09-03 / delete_selected_layout_then_restart | 删除成功但偏好仍指向旧 ID | 回退默认且报告，不启动崩溃 |
| P09 / X09-04 / opaque_state_roundtrip_and_budgets | 未知 provider/未来 schema/大载荷 | 合法未知字节保留；超预算准确拒绝不截断冒充完整 |
| P09 / X09-05 / legacy_migration_crash_is_idempotent | 新文件已写但标记前中断，再启动迁移 | 不重复生成新 ID；旧文件未破坏；仅新格式继续写 |
| P09 / X09-06 / layout_plan_preserves_extra_views_and_content | 现有 dirty Session 与布局外额外视图 | 计划保留它们且不触发 asset open/rebind |
| P10 / X10-01 / two_real_views_share_one_author_history | 真实双 SceneView 编辑、undo、不同相机/尺寸 | 一个作者源/历史；两个输出都正确且资源无验证层错误 |
| P10 / X10-02 / close_one_view_keeps_session | 双视图关闭一个再创建另一个 | SessionId/HistoryId不变，关闭视口资源最终退休 |
| P10 / X10-03 / author_and_run_views_do_not_alias | 同源启动 Run，两个窗口分别编辑/导航 | 绑定路由正确，运行修改不入作者历史 |
| P10 / X10-04 / material_flow_auxiliary_feature_matrix | 执行原 Material/Flow/Task/Project/Inspector 全部核心操作 | 未靠遗漏入口或删除测试获得通过 |
| P10 / X10-05 / factory_failure_no_ghost_and_rebind_keeps_old | 创建/attach/rebind 各失败点注入 | Root 与 Session 集合一致，原绑定未丢失 |
| P10 / X10-06 / input_capture_focus_and_ime_lifecycle | 拖拽/文本组合输入/焦点切换/回调关闭 | 焦点捕获按阶段释放，IME实测与未测分开报告 |
| P10 / X10-07 / new_integration_harness_has_no_old_dependencies | 检查完整新 GUI harness 链接/include | 无旧 Context/PaneManager/工具大类；harness 不作为第二产品安装 |
| P11 / X11-01 / query_replaces_own_registration | query 请求替换自身后继续访问捕获对象 | 旧 callable/code 在返回前存活；发布延迟 |
| P11 / X11-02 / batch_restores_with_fixed_extension_snapshot | 第一个 provider 请求更新注册 | 整批使用同版本；无 span 失效或半批新版本 |
| P11 / X11-03 / queued_command_keeps_identity_and_registration | 点击后改焦点、替换 registry、删除/复用目标 | 按固定策略执行原目标/原entry或拒绝，不漂移 |
| P11 / X11-04 / session_factory_worker_vs_owner | worker decode，延后 owner construct；角色准备失败 | 线程亲和正确；无半注册 Session |
| P11 / X11-05 / lease_covers_virtual_destructor_tail | 外部插件对象/任务/闭包析构顺序记录 | 代码卸载晚于最后析构返回，不只晚于成员销毁 |
| P11 / X11-06 / external_consumer_uses_installed_public_api | 清空源码 include 环境构建最小新插件 | 仅 install prefix 可编译；旧 ABI 拒绝发生在调用前 |
| P11 / X11-07 / runtime_sdk_not_tied_to_editor_change | 改变 editor 接口版本并构建独立 runtime 插件 | Editor 拒绝/更新范围准确；runtime 无不必要重耦合 |
| P12 / X12-01 / open_session_success_view_failure | Session 发布后注入视图构造失败 | 保留无视图内容，OpenResult 表达真实部分完成 |
| P12 / X12-02 / reload_waits_for_old_publish_and_checks_stamp | 旧保存仍发布、重载读入期间内容变化 | 无晚写覆盖新事实；未经审阅的新编辑不丢失 |
| P12 / X12-03 / close_all_late_cancel_destroys_nothing | A 保存成功、B 取消；另测第二张许可失败 | 所有 Session 仍在；A 真保存保留；许可释放 |
| P12 / X12-04 / close_last_view_three_choices | 最后视图选择 keep/close/cancel | 三种行为准确，取消前视图不被销毁 |
| P12 / X12-05 / layout_non_destructive_full_pipeline | dirty 双视图+额外窗口+缺插件+偏好写失败 | 内容/历史完整；报告实际结构与独立偏好失败 |
| P12 / X12-06 / save_all_deduplicates_sessions | 两视图同一源、一个无绑定源、运行中新开会话 | 按启动会话集合去重，逐项完整结果，无假全部完成 |
| P12 / X12-07 / model_load_is_not_insertion_success | 模型读取成功后源已换 HistoryId/插入预算失败 | 最终失败而非已插入；源未半改 |
| P12 / X12-08 / startup_menu_failure_prevents_publish | 各构造阶段/菜单注册连接失败 | create 返回错误，无成功但已经 fail 的对象 |
| P12 / X12-09 / exit_drains_before_destroying_dispatcher | 编码/运行/GPU/插件回调同时在途 | 无死锁/UAF/代码早卸载；真实退休责任结清 |
| P12 / X12-10 / only_new_product_path_and_zero_legacy_owner | 扫描所有构建入口、安装、AST和运行 target | 唯一新产品；所有限期桥与旧大类删除，零永久回落 |
| P13 / X13-01 / clean_install_consumer_matrix | 隔离源树/build tree 后构建三类 domain 与扩展消费者 | 所有公开依赖可解析，无私有头泄漏 |
| P13 / X13-02 / final_architecture_and_retirement_audit | AST/编译图/运行入口/安装包交叉检查 | 零 active 旧 owner/桥；没有 alias/宏/注释存档式替代删除 |
| P13 / X13-03 / real_platform_gpu_and_ime_matrix | 执行已冻结 Windows/Linux/desktop/GPU/toolchain 组 | 每项有真实状态和日志，未测不冒充通过 |
| P13 / X13-04 / long_running_capacity_and_latency | 持续保存、多视图开关、编译失败/重试、GPU 背压 | 所有集合有界且无寿命错误，统计与基线可比 |
| P13 / X13-05 / feature_and_failure_coverage_not_reduced | 逐条对照 P00 旧功能及所有 Q/X 测试 | 无删除功能/删断言/扩大N/A换取通过 |
