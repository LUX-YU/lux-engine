# D2-C 续行记录（2026-09-15）

状态：**D2_C_INCOMPLETE**，未授予 D3 准入。保留既有架构，不实现 Run。

可取得的检查点：`239028042f1ad6399526feae3ba50342b7c718f2`。
本次代码候选：`ab0e683bae2eb4570f2c5605a0037084c6193d54`。
开发分支：`codex/editor-d2-c`。main 未修改。

## 本次实际修复

1. `aad58157`：单独销毁 Inspector 后，再装配 Scene 会重复注册已有 Outliner。
   在 2390280 正常安装 SDK 上，真实 Scene、窗口及 Renderer 用例得到
   `ui.register / DUPLICATE_PANE_ID`，当时 views=3、revision=20。
   修正只恢复缺失 Pane；正在关闭的旧 Pane 返回 BUSY，原 owner 保留。
   Material、Flow 的重复装配也保持现有实例。前端重新打开已有文档时调用该装配协议。
   修后 Inspector 重建成功，另外三个 Pane 与历史保留，后续编辑、Undo/Redo 成功，
   最终 views=0、leases=0，正常退出。

2. `ab0e683`：真实鼠标关闭有修改的窗口后，保存／放弃／取消控件被 Project 的
   38 像素工具栏裁掉。修正把原协商控件放入独立模态弹窗，没有改保存和退出状态机。
   修后鼠标流程：Translation X 0→2，关闭→Cancel close，继续编辑为3，打开 Material，
   在空历史 Material 中 Ctrl+Z 后 Scene 仍为3，再关闭→Save changes，退出码0。
   输入、截图、保存前后真实文件和退出记录均保留。

最初新增测试驱动曾在文档尚未打开时解引用空 handle，已修正。
该次崩溃仅属于测试驱动错误；准确产品负例来自随后记录到 DUPLICATE_PANE_ID 的运行。
没有把任意崩溃或超时当作 Pane 问题的证据。

## 四条工作流

| 工作流 | 本次结果 | 仍需完成 |
|---|---|---|
| Pane 生命周期 | 真实 Inspector 单独销毁／重建、关闭期间 BUSY、重复装配、其余 Pane 与文档历史保留通过；既有隐藏期间修改／恢复继续通过 | 双消费者断开、旧排队通知与新实例组合的完整记录仍缺 |
| 裁剪／分页 | 保留已有64项分页、非随机容器定位、结构变化逆操作及 BUSY 手势保留证据 | 持续手势中滚动／分页／折叠／过滤、控件离屏后结束的完整用户流程未通过验收 |
| 实际取消 | 物理退出取消、继续编辑、恢复打开接纳及显式保存关闭通过 | 鼠标保持按下时 Esc 取消未验证；Computer Use 的 drag 只能一次按下→拖动→释放 |
| 跨文档撤销 | Scene、Material A、Material B 均有记录；真实 CommandRouter 的 A Undo／EMPTY 拒绝不改变 B、Scene 的版本和通知数；菜单捕获与注册失效拒绝通过。物理 Material 空历史 Ctrl+Z 不影响有历史 Scene | 文本框局部 Ctrl+Z 的完整物理组合尚缺；不能把三文档接口测试说成三文档物理测试 |

## 画布回归与原修正

缩放缺陷来自节点库默认 FitVerticalView：从很小的初始画布扩大窗口时缩放异常。
现有修正使用 CenterOnly，未升级或改写节点库。ID 冲突来自 node/pin/link 的独立业务编号
在同一 ImGui 交互空间相撞；现有 Pane 内映射保留完整64位业务身份。
本次补充两个画布上下文具有相同局部 ID、独立位置／选择、单 Pane 重建和坐标往返测试。
这些不等于物理缩放→平移→端口命中→拖放落点→保存坐标的整条流程全部通过。

| 原修正 | 证据 |
|---|---|
| R01 退出协商 | `editor.d2c.exit-review` 与保存系列；本次新增实际取消退出观察 |
| R02 编译完成／通知退役 | `editor.d2c.compile-notice`、Flow 实际编译／链接及完成通知 |
| R03 关闭所有权 | 在途保存／模型放置关闭、`close-signal`、新增 `pane-lifecycle` |
| R04 持久索引与作者内容 | 既有 indexed/read-only、codec、分区和未知 payload 保存回归 |
| R05 Scalar 四元数 | 既有 float/double 各12组、非法输入、预览与Undo/Redo |

## 六项准入条件

| 条件 | 当前状态 |
|---|---|
| G1 源码内聚 | 保留原迁移；本次只改共同前端和具体 feature 的装配，未增加架构层 |
| G2 定向正确性 | R01—R05 相关已执行回归保留，新增两项实际缺陷已修；C19 的组合证据仍待补 |
| G3 真实 D2 闭环 | PARTIAL：退出取消、局部空历史验证有进展；上述持续手势、文本组合和完整画布流程仍缺 |
| G4 异步与发布 | 正常 SDK 发布／恢复回归复用并重跑；专用诊断13个中断边界的旧有效原始记录保留，未重标成新诊断二进制 |
| G5 工程资格 | 独立干净副本、RelWithDebInfo all、随后no-op、安装迁移、33个安装测试、9个独立消费者和1个画布测试分别记录；不把数量相加成逻辑验收数。宏切换生成失效序列仍未完整补齐 |
| G6 代表规模成本 | 复用原五组独立进程样本。没有重测本次 Pane 恢复和弹窗的耗时；没有声称新代码带来额外加速。全局分配次数与独立 ProjectReady 延迟仍未测 |

原成本：100次主动绘制的中位数，Outliner/桌面4096对象22.708→3.923ms，
生成 Inspector 4096项181.281→8.897ms；256对象5.158→3.938ms，256项14.769→9.033ms。
两个规模静止 Outliner 均提交32行；Inspector 静止 binding 查询和目录重建为0。
普通 vector 元素手势100次更新保留一条历史、计费422字节；不是RSS或全局堆分配统计。
保存／resize 样本没有显示加速，不从进程时间推导 FPS 或 p95。

## 交付身份与复验

包内 `source/` 是候选的 Git archive；`reports/source-identities.json` 分别记录 Git blob OID、
Git blob SHA256 与 Windows checkout SHA256。正常 SDK、诊断 SDK、桌面工作 SDK 及消费者
二进制身份分别记录。`manifest.json` 覆盖包内实际文件。
2390280 原包和原日志保留；`qualification-2390280/`、`qualification-aad58157/` 保留此前资格日志。
`q-final-*` 对应本次最终代码候选。旧桌面窗口结束的未明原因不追溯改写；本次退出具有显式输入记录。

固定候选人工复验：使用随包 `verify/run-qualified-editor.ps1 -Project <已有项目>`。
1. 在 Inspector 拖动字段，保持鼠标按下，按 Esc，再释放。核对原值恢复、没有新增历史；下一次拖动能提交。
2. 持续手势期间让控件离屏或分页／折叠，核对统一结束政策、只产生一条历史、返回后没有旧草稿重提交。
3. 同时保留其他文档的可撤销记录，在文本框中 Ctrl+Z，核对没有穿透。

上述未测项保持未验证。没有合并 main、发布、冻结或进入 D3。
