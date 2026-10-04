# EC3 再核对与定向补齐

状态以唯一迁移账本 `ec3.recheck` 和本轮命令收据为准。本文件记录核对结论，不取代账本。

## 发现并处理的缺口

1. **SET-03/SET-06/SET-08/SET-09：目录与草稿准入错误耦合。**
   `SettingsContent::update` 原先总选最后一个 location；只有 `select` 完整成功才填写页面标签。
   真实安装 SDK 中，USER-only 页面面对 USER + USER_PROJECT locations 时得到 `settings.draft`，无草稿。
   现在先建立可供选择的目录，再从合法、可用的 scope 中选择；读取失败保留目录和错误。
   显式 scope 请求仍严格验证，不在 IO/BUSY 错误后尝试其他层。打开菜单后的维护安全点刷新目录；
   SDK 可调用同一 `refreshPages()`，稳定帧不查询。刷新只改变选择列表，不改变已编辑草稿的 entry、
   based_on、desired、applied、persisted 或冲突错误。被撤销的旧草稿仍明确拒绝，用户须显式选择或 Revert。
2. **SET-12/SET-13：只读 Save 拒绝前已经 Apply。**
   原 SDK 真实输出 `apply calls=1, applied=1, writes=0`，随后才返回 `settings.read-only`。
   已将只读发布准入移到任何 Apply 之前。保留合法 Apply 成功而文件发布失败时的已应用事实，
   不把两种情形混在一起。原保存协调器、Unknown、基线及退休算法未改。
3. **CMD-02/ID-01：部分消费者未使用模块的唯一固定描述。**
   SettingsView 现在向窗口构造和 Application 状态分发提供同一原 descriptor；后者不再每轮构造拥有型 ID。
   其余 11 个注册窗口的构造复用各自原工厂描述的规范名称，删除第二处固定类型文字。
   描述仍属于实际模块，不增加集中枚举或另一套元数据。窗口标题是呈现值；与菜单标签有意不同的标题保留。

两个真实修复前失败、首次测试夹具编译错误、首轮未登记新测试的 STRICT 拒绝分别保留。
夹具编译错误不是产品负例；规则拒绝通过补齐测试 target 分类修正，没有放宽依赖规则。

## 按 C0–C9 复核

| 批次 | 本轮核对路径与处理 | 未改路径的证据 |
|---|---|---|
| C0 | 原包 37 个文件哈希；实际实施分支；原工作区补丁及 main；本轮测试和安装变更单独记录 | 原声明、owner、target 清单仍按原 SHA 阅读 |
| C1 | CommandEntry 静态/动态 backing、代码保活及 registry 的调用/批次保护；本轮不改算法 | 原声明寿命、碰撞、八项 operation 负例与 SDK 记录 |
| C2 | ProjectContentSaving、工作区活动与保存/退出结果所有权；不把设置 UI 修补变成发布器重做 | 原真实 IO、版本衔接、Unknown、保存回归；本轮完整源码回归有其独立记录 |
| C3 | 检查固定工厂描述与 Pane 构造、Application 热路径；补齐上述 12 个窗口消费者 | 中性命令路径、Scene 枚举、原 shortcut parser 不变 |
| C4 | SettingsEntry/Document/Draft/resolve/prepare、ContributionSnapshot 的代码 pin 与 schema；不新建 registry | 原 core、外部插件及 codec 资格；新增范围受限与目录变更回归 |
| C5 | 实际 SettingsContent、SettingsView、启动解析和 WindowSettingsBinding；修复两项准入/导航缺陷 | 原字体、窗口模式与实际文件路径，新增源码及 SDK 回归 |
| C6 | CameraNavigation / 原 ECS、输出、退休路径未动 | 原实际双视口/borrowed camera 证据；本轮受影响视图再次运行 GPU |
| C7 | MetaUnit -> InspectorModel -> inja 与 support；只读/容器/作者与 Run 投影仍用唯一生成链 | 原安装生成器 25 个检查及暂停 Run 控件实测，不伪称本轮全部重跑 |
| C8 | 本轮修改的准入顺序、旧目录借用的身份复制、共享描述；新增测试的实际 STRICT 分类 | 无新 Context/Manager/缓存/并行 History；不补旧长测 |
| C9 | 各批准确 implementation SHA、全量构建/二轮、适用源码/SDK/GPU、公共头、独立记录与推送 | 历史归档仍可按固定 Git 对象验证，不改原 PASS/PARTIAL/FAIL |

这里是本轮原要求与实际路径核对，不宣称穷尽整个引擎潜在缺陷。具体运行成绩由命令输出支撑；
前一批运行之后的纯描述复用变更会重新构建和执行受影响路径，不能挪用旧 SHA 冒充最终运行。

## 仍保留的未测/免验

EC2 源码与 SDK 原生输入仍按用户安排延期；Linux、系统 IME、sanitizer、多显示器真实硬件和
旧性能样本保持原记录范围。P12 `PARTIAL_USER_WAIVER` 不改变。不因本轮设置修复扩大历史成绩。
原工作区的 ProjectBuilder 用户补丁未应用到实施仓；不修改 main、不合并、不发布。
