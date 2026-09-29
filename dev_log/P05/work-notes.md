# P05 施工记录

前置：`aad13c594afe33af69fcfba1b44790b4e9ad148d`，分支 `codex/editor-redesign-v4`，启动时工作区干净。
规范：原 V4 P05 与 `P05-input/START_P05.md`；启动包 SHA256
`7e9725cd3d04c75564113108a990f9073959aba112f423ed1c345d3f45c8bba7`。
本文件是施工说明，状态与删除责任仍只记入现有 migration-ledger/coverage/architecture。

## P05-A：核查结论

- SessionState 已是 binding、checkpoint、observed、EditGate 的唯一 owner。三类模型保持这条路径。
- ProjectPublication 当前执行多文件 journal、manifest 和 catalog 协议；不能把其 `EditorResult`
  直接解释为单目标的 Published/NotPublished。文件 write/flush/replace 原语应迁成唯一共享实现，
  旧 journal 继续调用这些原语，新 ProjectArtifactStore 只处理一个目标。
- Windows 原语是 FlushFileBuffers + MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)，POSIX 是 fsync + rename。
  不声明跨进程 CAS，也不声明多文件事务。替换成功是发布事实；目录持久性不能从文件 flush 推出。
- 规范化解析父目录及符号链接、限制到项目根内；Windows 使用大小写无关的规范地址。
  不支持硬链接多名称及可变符号链接别名并发，须在准入拒绝已知不支持情形并说明外部修改边界。
- ExecutionRuntime/TaskScope 已提供 scheduler、拥有结果、owner 收取与 join；无需新执行器。
  persistence 核心采用拥有工作项/完成结果接口，窄执行适配连接已有 CPU/Blocking scheduler。
- 新协调器只保证接入者；旧产品仍在 P12 私有桥范围，本阶段不使新旧 writer 并写同一文件。

## 实施顺序

A 已核查；B 写入协调及受控负例；C 服务/真实 IO；D 三模型 codec/采用；
E 预备重绑定及副本；F 完整验收和冻结。A～E 已实施；最终状态以 P05 收据和归档门禁为准。

## B～E 已实现的事实与实现取舍

- WriteCoordinator 是唯一目标顺序 owner，SaveService 只借用。编码前 reserve，按票据顺序发布；
  同 Session/binding 可衔接已确认版本，不同副本不继承。Unknown 保留字节/票据，必须退休写者并核验。
- SaveService 的私有 Operation 对应规范中的 SaveOperation；三个私有 PreparedRebind 对应具体
  SaveAsOperation 的预备采用角色。没有额外增加只转发同一操作的公共 owner。
- SessionStore / SessionState / History 的原唯一所有权保持。三个模型仅新增窄 PersistenceAccess，
  不新增 dirty/current/busy。稳定捕获先完成、再验证内容戳与绑定并取得已有 permit。
- SceneSnapshot 保留分区身份目录，否则结构候选构建会丢失 codec 所需的 ordinal→ID 信息；候选与
  捕获已接通同一目录，不建立运行期 Registry 适配。未知组件字节、根辅助 payload 和额外包条目保留。
- Scene 的包外壳复制提到 engine/scene/asset 的 copyScenePackage；旧 SceneSource 中算法体删除，
  只留下错误类型映射。ProjectPublication 的 write/flush/replace/read/hash 原体提到现有 storage 目录，
  新增一个必要 STATIC kernel 边界供旧 journal 和新 IO 共用；新 IO 不依赖旧 storage/editing target。
- Save As 只切换绑定及格式外壳，不重建作者域。Scene 的作者描述继续使用逻辑身份，编码按照当前绑定
  显式重建外壳；Material/Flow 更新 source asset id，图与历史 memento 不变。Flow 的已发号游标继续
  由 GraphTopology/FlowGraph 持有，测试覆盖 Save As→Undo/Redo→Undo/混合新建后的全部 PinId。
- encode 和 publish 分别复用 ExecutionRuntime CPU / Blocking scheduler，TaskScope 只收取传输事实；
  adoptCompletions 单独在 owner 调用，READING 时保留 AwaitingAdoption。没有捕获 live Session/Pane。
- 容量是逻辑内容计量，不是进程 RSS 上限。额外测量普通 new 调用仅覆盖测试 executable/STATIC 代码，
  不声称覆盖第三方 DLL 的分配。新框架在生产中没有加入性能监控 owner。

## 开发期失败证据

所有 P05-*.log 在最终快照的 logs/development 中原样保留。包括缺少 span 头导致的编译失败、
Unknown 后的版本衔接负例、结构候选丢失分区 ID 的真实 Scene 编码失败、以及精确 include 门禁
拒绝尚未登记的 required header。均有后续通过结果，不能以覆盖日志方式消除失败。

异步测试第一次构造 ExecutionRuntime 时未配置必填 timer.capacity，触发测试 take() 的 abort；
cdb 栈保留在 P05-F-async-debug.log。补齐测试配置后沿真实 CPU/Blocking/owner 路径通过，未修改执行器。

## F 最终核验安排

实现提交：8deaee9beaab431c1cfa82282206e7ff2f40d09b。validate_p05.py 只使用这一干净 tracked SHA，
先 tracked-snapshot，再显式 P05 configure、all -j4 -- -k0、二次 no-work、完整 CTest、详细旧回归、
SDK 重装、原八组消费者与新增持久化消费者、实际依赖负例、历史收据和独立 clean clone 闭包。
所有旧快照只通过其 implementation_sha 核验，不改写。最终冻结后另跑可搬迁 P05 收据门禁，
验证日志缺失/篡改以及前置记录缺失均失败。P06 不在授权范围。

补充边界核查：初始实现 96a90d0f1c072c787d6afeb71a73c70974242130 的完整矩阵已通过，
但人工检查发现 reconcile 的 foreign callback 没有异常 containment。增加真实调用负例后
出现未处理异常（P05-F-reconcile-before.log），随后添加窄 containment，保持 Unknown 及其字节责任，
返回 IO 失败；同一负例修复后通过（P05-F-reconcile-after.log）。最终实现为上述 8deaee9b，
全套矩阵重跑，不将初始提交的结果冒充最终提交验收。
