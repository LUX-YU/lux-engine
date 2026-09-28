# P01 验收：历史纯化与会话所有权

**状态：PASS，仅 P01。** 输入是 P00 验收提交
`a7234602aaeab86bb947214dcd2ea4eb1b314cfa`，最终实现提交是
`f167cb803011c9b37fa40edcc7cfc4652e7958bf`。验收记录单独提交。未修改 main，未推送，未进入 P02。

原始 V4 压缩包的 36 个文件与本地原件逐字节一致，校验记录见 `spec-input.json`。
唯一可继续修改的施工账本仍在 `.internal/editor-redesign/`；本目录中的三个 JSON 是验收时的固定快照，
后续阶段不得把它们当作另一份施工账本修改。`receipt.json` 的 `may_proceed=true` 表示门禁通过，
`continuation_authorized=false` 和 `stop_after=P01` 表示尚未获得进入 P02 的授权。

## 实现及唯一 owner

| 内容 | 唯一 owner / 生效规则 |
| --- | --- |
| 历史算法和当前 StateId | `editor/history` 中唯一的 EditHistory；原 prepare/commit/publish、预算、Undo/Redo 算法保留 |
| 新会话对象 | SessionStore 的 `unique_ptr<IEditSession>`；reserve/prepare 阶段不可见，publish 后才可查询 |
| 会话来源、基线、准入 | SessionState 的 SourceBinding、PersistenceCheckpoint、EditGate；不复制 History 当前状态 |
| 旧工具保存基线 | 每个工作副本的私有 LegacyPersistenceState；候选或复制源的 history/checkpoint 成对采用 |
| 插件代码寿命 | Store 槽外层 CodeLease；先析构会话、历史操作和源数据，最后释放代码 |

SessionId 区分 Store 域、槽位与代次，typed key 验证类型。访问引用只在 owner 线程的同步范围内有效。
EditScope 不可复制或移动；ClosePermit 和 BindingChangePermit 为 move-only，放弃仅解除准入限制。
关闭必须使用 Store 发出的、绑定当前对象和 ContentStamp 的许可；旧许可不能命中复用槽位。
Store 在描述/关闭准备的插件回调和回收过程中拒绝重入修改。

新模块不依赖旧 Editor、Context、UI、ProjectStorage、SceneRuntime 或私有过渡桥。
安装后的最小消费者只导入 edit_sessions、edit_history 以及系统运行库，见 DLL 导入记录。
本阶段仅交付会话基础及其真实行为测试，**未声称三个旧工具已经迁为 P02–P04 的业务 Session**。

## 到期删除和迁移

- 删除 `EditHistory::beginSave/finishSave`、`SaveTicket`、`ESaveOutcome`、`SAVE_STARTED`。
- 删除 `HistoryCreateInfo::initially_saved`，`HistorySnapshot::saved/save_pending/clean`，
  `SAVE_IN_PROGRESS/STALE_SAVE`，以及实现中的 saved、pending、request；原调用全部迁移。
- F001–F005 的五个纯历史文件移至 `editor/history`，不留转发头或命名空间兼容别名。
- 七个重点对象的 AST 对比只有五个旧历史成员消失、八个限期 checkpoint 字段新增。
  历史 46 个成员中的 41 个保持原签名迁移；证据见 `history-member-proof.json`。
- `EditHistoryTarget` 析构实现留在旧 editor_editing 的独立 CPP，不把领域编辑协议拖入纯历史库。

`editor/transition/LegacyPersistenceState.hpp/.cpp` 是唯一登记的桥，编入旧 editor_editing，不安装。
消费者限定为 TAssetSave、Scene/Material/FlowForge 和对应旧产品测试；新模块不得反向使用。
桥仅保存一个 checkpoint 和单个在途保存票据，不做 IO、不拥有 Context、不复制历史算法。
它和八个旧 Impl checkpoint 字段、旧工具查询适配均最迟 **P12 删除**。枚举按项目风格采用
`ELegacyPersistenceOutcome/ELegacyPersistenceError`，不另留旧名称别名。

完整的新增、修改、迁移列表及文件校验和见 `files.json`（68 项）。

## 行为证据

| 门禁 | 实际观察点 |
| --- | --- |
| X01-01 | P01 API/成员检查及 AST；原历史执行、准备失败、预算、无变化、Undo/Redo 保留 |
| X01-02 / Q06 | 不同 Store、错误类型、回收复用槽位、旧 key 和已消费许可均拒绝 |
| X01-03 / Q07–Q08 | 撤销回保存点恢复 clean；相同字节的新 StateId 仍 dirty；旧 HistoryId、绑定和发布顺序拒绝 |
| X01-04 / Q10 | 合法 scope 引用可编译；复制和移动因 C2280 deleted constructor 失败；重入及移动/放弃许可行为验证 |
| X01-05 | Material、Flow 实际文件替换失败保持原基线，重试成功更新基线；损坏的重载候选保留原内容/历史/基线 |
| X01-05（Scene） | 保存重试、部分发布、live S2 与保存捕获 S1 分离；精确检查在途票据，未知 payload 原样保留 |
| X01-06 | 无效 lease、槽位容量和候选身份失败不发布半会话；析构顺序为历史操作 → 源 → 代码 |
| Q09 | 历史 prepare 校验/准备预算失败与旧工具候选失败不产生半次采用；原场景编辑回归继续执行 |
| Q45 | 实际直接/传递依赖图、负向夹具、私有桥禁入新模块；使用 P01 阶段门禁 |

相关 Q 的记录限定于本阶段已有类型和受影响旧产品；P02/P08 的业务整合以及 P13 全面验收不提前标为完成。
没有删除原测试或以计数代替上述行为：原 P00 的 31 个 CTest 名称保留，新增两项 native 检查及两种真实
Scene 保存模式。完整结果 **35/35**；其后顺序执行 **5 组安装消费者、6 项测试**，包括外部插件和 GPU 消费者。
主构建及全部消费者都按 `all -j 4 -- -k 0` 构建，第二轮无新增工作。命令和日志哈希见收据。

## 仍然失败的 P00 缺陷

| 缺陷 | 原样复测结果 | 后续责任 |
| --- | --- | --- |
| C01 / Q31 | 无效布局被拒，但窗口 visible 从 0 变成 1；退出码 1 | P09 验证、P12 接入 |
| C03 / Q38 | QUERY 回调内替换命令时提前释放代码；退出码 1 | P11 |
| C04 / Q42 | 注入连接失败后 create 仍成功；退出码 1 | P12 |

三项仍明确为 **FAIL**，没有包装成通过的 CTest，没有修改复现判定或顺手修复。
P01 PASS 不表示这三项已解决。它们的失败日志与原责任均保留。

本轮另外发现并修复了两个验证问题：编译负例脚本未替换 Windows 路径，以及旧包在安装生成之后登记新依赖。
修复前失败日志保留，修复后在最终实现提交完整重跑。验收脚本首次误读架构报告中的不存在字段，也保留了该失败日志；
修正为核对真实输出格式和实际 `--stage P01` 参数后通过，没有改变产品断言。

未执行 Android、clean-clone foundation/closure 全面认证、完整人工 IME/桌面和性能专项；这些不属于 P01 必测门禁。
本轮没有修改 modules 公共头，因此不涉及三个安装前缀的 modules 头同步。没有新增第二套历史、资产格式或 P02 模型。

`verify.py` 复核命令哈希、原测试保留、P01 到期账本、AST 差异和三项失败事实；它不替代运行证据或所有权审阅。
