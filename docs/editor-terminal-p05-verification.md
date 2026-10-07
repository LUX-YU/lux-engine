# Editor 终态规范：P05 Event-driven Project RAII

P05 gate：PASS。实现为 `1e98089c9` 及补正 `02eec3183950a74e3a9f36e1c71c46cf00cb7bdd`，
前置 P04 验收 `4fac8d7e1`。本记录独立提交；按用户授权继续 P06/P07，不等待中间复审。

LuxEngine 使用原 Object Event 处理 Open/Create/Close 与应用命令。后台只持有输入，结果经
ObjectScheduler 的受保护 continuation 返回。request serial 保证 latest-wins；陈旧及取消完成不发
伪失败。候选在 Event 中离树构造，safe point 使用唯一 Root transaction 原子采用，在通知前发布
新 Context/mount，随后先销毁旧 Pane 再销毁旧 Context。无 scope join、transition polling 或退休 Context。
文件发布事实、global Pane、外部重新登记的 Pane 和 late completion 分别保留原责任。

删除 ProjectTransition/status、advanceProject、关闭等待列表和宿主回调 BUSY 入口。产品状态 Pane
改为事实信号，菜单的 close/cancel 使用同一原 Command 路径。运行接口清理与最终 runtime shutdown
资格按规范归 P06；本阶段成绩不能替代 P06 的 outstanding-work/sanitizer 验证。

## 实际验证

固定最终 tracked SHA，ValidateTrackedSnapshot 通过。独立干净检出复用构建目录做增量验证，
不宣称冷构建。新 SDK 前缀为 `install/Framework-terminal-p05`。

| 范围 | 实际结果 |
|---|---|
| Editor | all -j 4 -- -k 0，二次 no work；60/60 CTest |
| PLAYER | all、no work；40/40 CTest |
| 安装 SDK | 18/18，逐个公共头 C++20/无 RTTI 编译 |
| 最小组件 | Project、Scene、services/tasks、ObjectScheduler、TaskScope 各 1/1；Object 2/2 |
| 依赖负例 | Context→UI、UI→Editor 实际非法边被拒绝，去边恢复，恢复后 all/no work |
| 实机/文件 | 实际项目切换、渲染、resize/minimize、阻塞 worker；安装产品中文路径创建/重开/WM_CLOSE |
| 归档 | 中文/空格路径搬迁通过；删除及篡改真实 SDK 日志均被拒绝 |

L01–L14 由 project_lifecycle、project_requests、desktop、ui.root 和 ui.transaction 的实际行为覆盖：
global Pane 持续存在；关闭与替换不等 worker；B→C 包括已入 Object 队列及候选工厂递归请求；
文件/插件/Root 拒绝保留 A；Create 发布后失败保持 published；旧 PaneHandle 不追随新登记；
真实 Root 无焦点命令到达 LuxEngine；已有 TextEdit Undo 优先行为保留；通知观察新语义 owner，
通知中的新关闭意图不会被外层清空。Context/Pane 按实际 allocation 检查析构顺序。

## 首次失败与补正

旧全局 dispatch 检查拒绝 Event 内离树控件组合，也拒绝无关候选析构。修正只允许组装未挂载候选及
清理不被访问的对象；挂载 Root、已 parent 的重接和当前 callback 目标仍受保护。新增真实 Event
回归锁定此差别，P01 queue/scheduler 算法未改。最初扩大 ancestor borrow 检查还阻止了合法的 deferred
owner 子树替换；全量 ui.root 捕获后撤回该多余检查，保留原 deferChange 保护与全部断言。
另一个首次失败来自同名 C→C 测试错误地使用累计析构数，已改为该 Context 实际服务的寿命标记。
所有首次失败保留，最终矩阵绑定补正 SHA。

证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p05/verified-evidence/`。
957 个文件、65 条真实命令；manifest SHA256：
`e95a2c883f62410089b5929e1088fe64243757a02f8a8d84369073a9d031e838`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。

原 Context/Pane 排版差异仍未提交。LuxEngine 重写前原字节与用户补丁保存在本阶段 protection；
旧计时语句所属实现将按 P06 删除，不把补丁擅自当作业务变更。ProjectBuilder 补丁未应用，main 和
历史记录未改。原生输入 NOT_RUN_USER_DEFERRED，Linux/IME/旧性能延期不变；sanitizer 留 P06 实测。
