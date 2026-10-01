# 验收、阶段交接与后续质量约束

## 1. 验收基线与权威

以每一阶段最后一个实际测试的实现 SHA 为资格对象。该 SHA 之后单独写证据提交，避免收据自引用。

P11 输入以当前结构交付及核验结果为准；P12 输入必须是已复审的 P11。原 `dev_log/P10Q`、`P10Q-structure` 和更早快照保持原字节；新的 scope 决定另记。

原工作区和独立检出分别记录，不把“资格检出干净”说成“用户工作区无修改”。ProjectBuilder 的原用户补丁迁移时应保存新旧路径和 hash，未经用户明确合并不得纳入阶段实现。

## 2. 最终运行顺序

每阶段内部按影响范围测试，不每次迁文件都跑完整性能矩阵。阶段末统一：

1. `git diff --check`、提交集合和实际未提交状态；
2. 正式阶段 gate（P11 或 P12）及 `LUX_EDITOR_LAYERING_MODE=STRICT`；
3. 最终实现的完整 Windows 构建，二次无新增工作；
4. 完整适用 CTest、独立 CPU 配置、PLAYER；
5. 全新安装前缀，原适用消费者、新增命令/扩展/产品消费者和公共头解析；
6. 原与新增实际依赖负例、concept 和 operation 特殊成员负例；
7. 真实文件 IO、实际插件、正式新桌面/双视口/GPU验证层/原生输入；
8. 必要生成输出删除后重建与正确 provider 检查；
9. source/target/install/runtime 残留检查；
10. 冻结证据的迁移、缺失/篡改拒绝和恢复正例。

CPU 配置不被 GPU/工具链运行测试强迫依赖环境；关闭测试能力不能关闭本应交付的生产功能。所有日志记录真实配置、设备、包前缀、工具和退出码。

## 3. 层次与依赖负例

以下列为最低主题，不为每项创建一个测试程序；复用既有架构检查器和失败原因验证。

| 编号 | 非法边/行为 | 正例恢复 |
|---|---|---|
| N11-01 | commands 纯策略 include 旧 EditorContext 或 UI Menu 实现 | 使用纯描述/目标契约。 |
| N11-02 | activities/sessions include workbench/ViewHost | E4 组合发布与显示，E2只返回内容结果。 |
| N11-03 | 具体 SessionFactory 反向进入 application Extension 总表 | 工厂依赖自己的 E2 contract，App安装它。 |
| N11-04 | 新插件直接 include pinclude/sinclude/TestAccess | 只用安装公共契约编译并运行。 |
| N11-05 | authoring 模型因新工厂拉入 Process/Compiler | 工厂实现归 E2，纯源仍无执行依赖。 |
| N11-06 | 旧 ABI 被强制视为新接口调用 | 实际 header/version mismatch 拒绝。 |
| N12-01 | 应用新入口仍 LINK_ONLY 链接 editor_context/editor_ui | 完全移除原 provider，真实链接通过。 |
| N12-02 | Layer 借 target alias/prefix 黑洞掩盖反向依赖 | 跟踪真实 provider、PUBLIC/INTERFACE/生成头。 |
| N12-03 | Material 使用 viewport 时链接整套 Scene UI | 只依通用 viewport。 |
| N12-04 | TaskMonitor 通过 tasks_ui 引入 ImGui | E2 editor_tasks 独立消费者。 |
| N12-05 | engine/modules 或 PLAYER 反向 include/link Editor | 无 Editor 编译输入和运行依赖。 |
| N12-06 | 新前缀消费者从旧源根/开发SDK找到旧头 | 清洁搜索路径与新 install manifest通过。 |
| N12-07 | 以运行时 option 或 #if0 留旧可运行产品 | 旧 source/target/安装物实际删除。 |

只有指定规则拒绝且同夹具去除非法边后通过，才计合格。缺头、缺包、网络失败或构建器缺失不计负例成功。

## 4. 五条必须用真实系统组合验证的链

### A. 命令与注册

真实 Menu/Shortcut → 固定 invocation → query/execute snapshot → 对应真实 Session 或活动 → 结果。中途变化焦点、注册、身份与代码 owner。

### B. 内容工厂与保存

真实三模型 decode → owner Session/role安装 → 编辑 → Save/SaveAs/Export → 真实文件 → 关闭 View → 迟到结果结清。检查文件事实与 checkpoint 不混。

### C. Run/编译/退出

作者编辑、Run 单步、编译和输出同时在途，Exit 决定与保存完成交错。原 runtime 和 Process 被驱动到真实终态；单步晚读与代码 payload lifetime保留。

### D. 布局与恢复

真实 Root 中 dirty 双视图+额外窗口 → 布局加载/纯计划 → provider失败/坏dock → 无副作用；随后成功应用 → 偏好失败独立。恢复只从独立manifest打开内容。

### E. 产品与安装

只使用新安装前缀启动 `lux_editor`，通过正式菜单打开/编辑/保存/运行/关闭。新外部 Editor 扩展和独立 runtime 插件分别工作；不借测试专用旧 Editor。

## 5. 删除阶段如何保留测试

### 5.1 没改的核心算法

原模型、保存、运行、交互、P10Q 共享字节与容量测试尽可能保持源码/断言。迁移 include 路径不能破坏命名空间/ABI/序列化身份。

### 5.2 旧产品测试确实需要迁移

记录：原测试路径和 blob、旧接口、用户行为、原断言、正式新路径、新观察和测试名。

当旧行为是“调用旧 Editor 读取业务结果”，用新 EditorApplication/activities 的正式查询检查相同事实；不要因为旧类型删除而放弃验证，也不要通过一个永久旧 TestAccess 包装继续运行。

移植测试允许合理 API 更名和状态拆分，但 must-observe 不能减少。特别保留完整源编码、History/current/dirty/绑定、磁盘事实、视图状态、身份代际和代码销毁顺序。

### 5.3 历史证据

原 frozen FAIL 继续保留。当前后继实现通过是新增证据，不是把旧文件中的 FAIL 改成 PASS。若测试声明变化，说明为什么保持原契约含义，不能将 C04 从 startup 换成 exit。

## 6. 平台与性能的终态表达

| 项目 | 当前可接受表达 |
|---|---|
| Windows 功能/安装/GPU | 实际命令通过，限定设备/工具链和运行范围。 |
| Linux | NOT_RUN；完成源码/构建静态审查，不宣称可运行。 |
| 系统 IME | 未做候选/组合/提交实测则 NOT_RUN；Unicode注入不替代。 |
| ASan/UBSan | 只有真实匹配构建与运行才记录通过；无资格仍NOT_RUN。 |
| 原50k长测 | 原PARTIAL和66/100保持；本轮不补样本、不用于阻塞。 |
| 轻量性能回归 | 只对修改路径运行有界容量/复杂度/字节owner等短测试。 |

保留 P13 的后续平台资格定位，但不能以 P13 为名延期清除旧源码。

## 7. 证据组织

复用唯一账本：`.internal/editor-redesign/` 的 `closeout` 节点。建议字段可见 templates；不得创建第二套持续变化的 registry of receipts。

每阶段冻结：

```text
dev_log/P11/ 或 dev_log/P12/
  README.md
  receipt.json
  source-map.json
  behavior-map.json
  removal-plan.json
  dependency-map.json
  logs/
  failures/                  # 真实开发失败，不冒充最终通过
  protected/                 # 用户改动来源/补丁/验证，不写入生产代码
  archive-probes/
```

这是证据内容类别，可合并已有同义文件，不要求新建多个几乎为空的 JSON。

必须有实际 `implementation_sha`、命令 argv/目录/环境摘要、开始结束和退出码、相对日志路径/内容校验，以及实现/证据提交区别。

验证器读取冻结归档和固定 Git 对象，不依赖生产机器 E: 绝对路径；缺少真正必需日志、修改真实日志、实现SHA不匹配都拒绝。不能因目录移动就改写历史check脚本的原义。

## 8. P11 交付声明模板

```text
P11 已完成，停在 P11 待复审。
实现：<sha>；验收：<sha>。
正式命令/不可变注册/两阶段工厂/外部SDK已实际接通。
C03 当前契约：<结果及新旧调用路径>；历史FAIL保留。
本阶段删除：<具体协议/文件/target>。
P12最后消费者：<精确列表/原因/替代批次>，没有新增兼容桥。
Windows实际资格：<明细>；Linux/IME等按实际范围记录。
未修改main/用户补丁/原历史快照；未进入P12。
```

不能只写“接口完成、测试通过”，必须列出仍存旧 executable 的事实。每次交付附实际可供用户查看的新工作区路径与 HEAD；原工作区仍在旧 SHA 时明确说明，用户补丁是否已应用也单独报告。

## 9. P12 交付声明模板

```text
P12 已完成，唯一产品已切换，停在P12待复审。
实现：<sha>；验收：<sha>。
安装的lux_editor来自<正式bootstrap>；无old/new回落。
九个旧根已清空；旧Context/Editor/注册/保存桥及旧安装入口清零。
已保留的正式旧名target及理由：<清单>。
C01/C03/C04当前等价契约通过；历史原FAIL仍保留。
完整功能/真实IO/双视口/输入/退出/插件/SDK/PLAYER：<结果>。
Linux/IME/旧性能未测或PARTIAL没有被改写。
用户修改、main、数据兼容与历史归档保全。
```

存在待删旧 owner 或实际仍加载旧 DLL 时，不能使用上述“清零”措辞。最终六目录指提交中的 tracked 源码结构，不授权删除用户原工作区的未跟踪文件或缓存。

## 10. P11/P12 之后继续遵守

- 不新增顶层 adapters/core/services/tools 混合分类，五层职责固定。
- 不让窗口成为作者内容、保存、编译、Run 或代码装载 owner。
- 不为一个单一具体服务自动创建 I/Port/Adapter 三层。
- 动态贡献只擦除一次；编译期复用用 concept约束小算法，不模板化整个应用。
- 通知用 LuxObject，返回值用准确Result，有限工作走Process，资源寿命走原Runtime。
- 构造建立必需依赖；回调/异步/代际/外部IO边界保留复查。
- 保留静态库的实际独立闭包，不用全符号导出/whole-archive隐藏缺依赖。
- 废弃实现随最后消费者一起删除；不会再生成“临时但没有期限”的第二体系。
