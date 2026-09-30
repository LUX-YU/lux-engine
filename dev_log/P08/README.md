# P08 独立交互与离树 UI 验收交接

本轮仅执行 P08。前置 `833d7efb18f8349cda9968ac3e4ea24ff2b54c60`；实现 `6a08a75a162a056a4d1cef1777eae9759f593b3d`。验收绑定该提交的独立干净检出，显式 `LUX_EDITOR_MIGRATION_STAGE=P08`。实施分支为 `codex/editor-redesign-v4`，不进入 P09。

唯一可变施工材料仍为 `.internal/editor-redesign/`；本目录为冻结证据，不作为第二份可变账本。原 V4 P08 与本次包 reference/v4 原件逐字一致；本次输入压缩包、展开原件和哈希归档于 spec/。原 P00～P07/R1 快照不改写，按各自 implementation_sha 重新核验。

## 交付行为和所有权

三个工具分别增加独立 STATIC interaction target；view_api 为 INTERFACE target，没有新增 DLL。交互只借用 typed Session access/key；Scene 可额外持有借用原 RunStore 的 RunInspectAccess 值。SessionStore 仍是三种作者模型的唯一 owner。

Begin 捕获原 ContentStamp；Preview 交换临时领域批次，不修改编码、current、observed、dirty、checkpoint 或历史；Commit 严格校验起始戳后调用一次原领域 apply；Cancel 只清理覆盖。读取和扩展载荷清理使用原 Session gate。Material/Flow 的节点坐标仍由正式领域编辑持久保存，并经过 undo/redo。选择失效检查使用原作者／运行身份，不保存跨帧可写 Registry 或字段指针。冲突保留待处理手势，不能偷偷换基线覆盖其他编辑。

| 对象 | 唯一拥有关系和寿命 |
|---|---|
| SceneInteractionGroup / MaterialInteraction / FlowInteraction | 拥有选择与 optional 临时领域批次；不拥有 Session；不可复制、不可按值移动 |
| DetachedView | CodeLease 与 unique_ptr<Pane> 构成拥有单元；Pane 的成员或 unique_ptr 是子节点唯一 owner；move-only |
| PreparedAttachment | 唯一拥有私有准备记录，记录对 Root/Pane 的非拥有引用；move-only；Root/候选先析构会使其失效 |
| Root | 只登记活动窗口、路由、焦点、capture 和准备观察关系，不 delete Pane/Element |
| LuxObject | 保留原非拥有父子链、亲和线程和活动回调约束；仅合并摘链算法，没有添加通用拥有语义 |

DetachedView 移动赋值以完整拥有单元替换，旧节点全部析构后才释放旧 code。仍挂载时直接销毁拥有单元是契约错误，Host 必须先卸载并移交资源退休责任。准备提交先将记录转为 commit 的局部 unique owner，通知回调释放调用方令牌也不会破坏正在遍历的记录。

## 身份与挂载协议

SceneObjectRef 继续是 Session／History／WorldObjectId，RunningObjectRef 继续是 Run／SceneInstance／Entity 代次。InteractionGroupId 表示共享选择与手势，不携带相机。ViewId 为 Host 域＋slot＋generation；ViewRestoreKey 为独立稳定键；ViewTypeId 直接复用 PaneTypeId；PaneId 仍只是 UI 活动登记名。不存在这些域之间的强转或用恢复键命中新的活动对象。

Pane/Element 增加真正 detached 的 owner-thread 构造和 `attachedRoot()` 空观察语义。离树可组装成员子对象、修改本地标题/内容/布局属性；不会占用 Root 注册、焦点或路由。未挂载 focus/capture 请求返回 false，卸载返回 NOT_ATTACHED。测量和实际 ImGui 绘制仍要求已挂载，不将旧 root() 的引用前置条件改为静默成功。

挂载顺序：Host 先准备拥有容量，Root 检查身份/结构/容量并预留索引；安全点验证版本，接管全部关联；最后通知。只有 commit 成功后 Host 才发布 ViewId。通知失败由 AttachmentCommit 的 SignalDelivery 报告，已提交事实不回滚为“从未挂载”。

卸载顺序：安全点先清空路由、焦点、capture、menu、deferred change 与活动登记，再摘除 Root 的非拥有父链，然后通知。通知期间仍冻结子树；close/show/focus 通过 ViewRequests 携带 ViewId 排队。资源责任按原 P06/P07 lease/retirement 交接，不以 UI 节点消失冒充 GPU 已退役。实际桌面 Host 在 P10 实现，本轮测试 Host 不进入生产目标。

## X08 和相关 Q 的证据

| 验收 | 实际场景与记录 |
|---|---|
| X08-01 | scene-interaction、material-interaction、flow-interaction：真实模型 Preview 冻结/编码不变，完整身份和保存基线不变，取消零历史，最终一次历史；持久节点布局 undo/redo；原 gate 重入拒绝和冲突不覆盖 |
| X08-02 | run-selection：真实 RunStore/SceneRuntime 中同数值 Entity、不同实例、停止/槽位复用；作者未改变；Scene 作者删除/重载同 UUID 拒绝旧 History 引用 |
| X08-03 | detached-views：实际 Root/Pane/Element，完整构造、未挂载析构、子节点构造中途异常、完整候选后续失败，活动计数和焦点不变，节点/code 各释放一次 |
| X08-04 | detached-views：Host 拥有/通知容量、Root 注册容量拒绝；移动赋值旧节点先于 code；过期准备拒绝；Root 先销毁；多次重新挂载复用存储 |
| X08-05 | detached-views：draw/signal 中请求 close，回调返回前仍存活，安全点后一次释放；后通知 FULL 保留已挂载事实；递归准备 BUSY；移除通知中释放令牌安全 |
| X08-06 | detached-views：离树 attachedRoot 为空，焦点/capture 明确拒绝；成员及动态子树组装、挂载、卸载、重挂均通过真实底层 UI |

Q10 复用原 Session gate/permit 测试并增加真实交互重入；Q26 对应 X08-02；Q45 包含 19 个新真实依赖/源码负例，每个命中预期规则，同夹具修复后通过。

Q02 本轮验证共享作者与独立选择组及原共享投影；完整双视图桌面相机行为仍属于 P10。Q29 执行原 Runtime/Run、投影、背压、后端和 GPU 消费者；新产品 GPU/像素资格仍属于 P10/P13。Q34 本轮完成底层离树/真实 C++ 构造和容量失败，完整桌面 Host 准备在 P10。Q44 验证部分初始化清理和声明的容量失败，OOM 仍按项目终止契约，不声明分配耗尽恢复或完整 bootstrap 资格。

## 工程验收

最终完整 CTest **139/139**，PLAYER **11/11**；原十一组 SDK **54/54**，新增一组 **2/2**，合计十二组 **56/56**。最终结果由 receipt.json 逐命令记录及 check_receipt.py 门禁核验。完整构建使用 `all -j 4 -- -k 0`，第二次无新增工作；随后顺序执行 CTest、SDK、PLAYER 和原 GPU/旧产品回归，未并发构建与实机测试。

原 133 项测试名称和全部已有 C++ 测试体保持，新增六项行为/依赖测试。原十一组 SDK 和八项 operation 特殊成员编译负例保留；第十二组通过真实安装头/库验证三类交互与实际 detached UI。不是只靠测试数量判定。安装 traits 继续拒绝两种 operation 的四个特殊成员，保留 unique_ptr 转移、Flow 借用、拥有产物及固定链接重试。

modules 公共头同步 Debug/include、RelWithDebInfo/include、Android/lux-engine/include 三前缀并记录哈希；Android 仅同步头，不执行构建资格。

check_receipt.py 只读取归档文件和固定 Git blob；原生产路径仅作为命令记录，不作为证据读取位置。生产路径不可用仍通过，缺失或损坏归档必须失败。全部文件、Git blob 哈希和日志哈希见 files.json、FILES.md、artifacts.json。

## 删除、暂留和失败

- 合并 LuxObject 原析构摘链体与新 detach；没有两份父链算法。RunInspectAccess 原 contains 体移入 RunStore 的唯一查询能力，没有复制运行规则。Root 原维护压缩体成为私有共用实现，避免挂卸历史堆积。
- 实际编译数据库门禁发现 UI 的 sinclude 被传递给 view_api；已收窄为 UI 私有目录，仅 VulkanBackend 的原目标显式使用。没有放宽 PRIVATE_COMPILE_INCLUDE 规则。
- 新路径没有 EditingGuard、重复 busy、即时逐帧作者写入或 rooted 构造；没有新 Session/History/Runtime/执行器/通用票据管理器。
- 旧 rooted 构造、旧无条件 root() 和原析构通知顺序仅供现有产品及其回归消费者，最迟 P12 删除。旧工具 EditingGuard/busy/finishing 仍有旧 UI 和暂停 Run 调试消费者，限定这些消费者到 P12；不声称本阶段已完成产品切换或全仓删除。
- C01 仍归 P09/P12，C03 归 P11，C04 归 P12；原断言和 FAIL 证据不改写。本轮再次运行并在 receipt 中记录，P08 新问题不挂旧编号。
- 原冷构建失败、P06 生成依赖修复记录保留；本轮干净源码检出复用已有构建树，不宣称全新冷构建资格。development 保留本轮开发失败、私有 include 门禁失败及被主动中止的先前候选资格日志，只有 receipt 的最终实现 SHA 是验收依据。
- `ProjectBuilder.cpp` SHA256 保持 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，不计入实现或验收提交；main 未修改。

实现与本验收记录分别提交并正常推送，停在 P08 等待复审。
