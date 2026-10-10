# MA07：Action/Menu 声明与贡献收敛

实现提交：`80f82db40602815a80bf060867889a3bdf1bc44b`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。
菜单行为和安装消费者通过；完整 Editor 资格为 **PARTIAL**，不能报告全量通过。

## 实际职责和删除

`CommandId` 仍为唯一命令身份。`ActionDescriptor` 拥有默认标签、快捷键与 checkable 声明，
不持有 handler、enabled 或 checked 状态；这些事实仍通过原 Command QUERY/EXECUTE 路由。
`MenuAction` 只引用该身份，`MenuSeparator` 和 `MenuNode` 表达菜单布局。
删除旧 `MenuItem` 及在菜单树中另查快捷键的算法，没有同义别名。

Root 在安全点准备完整不可变菜单及已解析的索引，成功后一次采用。失败保留旧定义；
稳定绘制按索引读取，不重新解释名字。已接纳命令独立拥有其 CommandId，菜单替换不丢弃它。
仍使用上一闭包的唯一安全点队列、同步查询、延迟执行、固定批次与代际目标校验。

Editor 的 `EditorMenuComposition` 是拥有型声明值，`composeEditorMenu()` 在现有 UI 组件中实现。
菜单、组和动作的 before/after 约束先拓扑排序，再按组、优先级和 canonical ID 确定顺序。
产品 main 使用同一公开贡献入口；没有新增 Registry、Manager、库或 Context→UI 依赖。
Root 不解释 Editor 贡献规则。外部贡献源只交付值，不把 DLL 回调保留在菜单中。

## 实际验证

- 独立 clean tracked 源码通过 `ValidateTrackedSnapshot`；Editor/PLAYER 使用已有增量构建树，
  不能记作冷构建。两者全量 `all -j 4 -- -k 0` 通过，第二轮均无新增工作。
- 完整 Editor CTest **169/170**。`render.transfer_idle` 的 lost 分支缺少最终 PASS 标记，
  输出停在 `reached transfer idle boundary`；原断言保留，未自动重试改绿，原因仍未确认。
  这与先前已记录的现象相同，本次日志独立保存，不改写之前的失败或通过记录。
- PLAYER **96/96**；原测试名称保留。开发阶段受影响 UI/Host/GPU **19/19**。
- 新 SDK `D:/LuxQualification/ma07-action-install`：Action/Menu **2/2**、UI composition **1/1**、
  Framework **15/15**。全部重新配置、构建、运行；五个新增相关公共头独立编译。
- 原真实 UI transaction 测试保留，并增加两个实际 ImGui 菜单点击和快捷键共同命令身份、
  同步 QUERY/延迟 EXECUTE、动态禁用、快捷键改配、去除菜单位置后仍可快捷键触发、
  元数据清空后已接纳命令仍完成、回调中替换拒绝及候选失败保留旧状态。
- 贡献测试执行 72 组排列，覆盖确定性、跨组锚点、重复身份、未知引用、锚点与环。
  首次父环夹具遗留一个无效兄弟锚点，实际返回 UNKNOWN_ANCHOR；原输出与源码保留，
  仅清除该无关锚点以独立验证 CYCLIC_ORDER，没有放宽生产判定。
- 真实安装 DLL 在 host 贡献之前/之后装载，声明后卸载，再组合菜单、清除来源值并通过公开输入执行。
  两种装载顺序生成相同布局，执行不依赖卸载后的代码。
- 四项真实 SDK 编译负例拒绝旧 `MenuItem`、元数据 `enabled`、`handler` 和引用项 `label`；
  每项移除非法用法后重新构建并运行成功。未使用隔离声明替身。
- 编译/链接输入核对没有借用源码生产私有头、旧构建 DLL 或 legacy。
  四个修改的 modules 公共头逐字节同步 Debug、RelWithDebInfo、Android include；Android 仅同步头。
- 六处用户差异哈希保持且未纳入提交；ProjectBuilder 补丁仍独立、未应用。

## 证据和范围

外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma07/actions/evidence`。
94 个文件、49 条实际命令；manifest SHA256：
`06b7e3c6aafa342ddab4b782ff2ebc8bff2731f4f96b317261a29a999f23605e`。
中文/空格路径搬迁、真实必需日志缺失和篡改拒绝均验证通过。

MA07 §12.13 的行为由本记录和前一安全点记录分别按实现 SHA 支撑，不合并成虚构的单次运行。
完整机制整改尚未完成；MA06/MA08 的来源适配与实际图呈现资格、MA09–MA11 仍待收尾。
LR08/Linux 仍未测未满足；原生输入按用户决定延期。IME、sanitizer、历史性能和其它历史未通过
范围保持原判定，不以当前通用 UI/GPU 测试代替图编辑器或新实机资格。
