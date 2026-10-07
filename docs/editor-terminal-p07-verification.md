# Editor 终态规范：P07 按组件拆分错误目录

P07：PASS。最终实现 `fb468d062f2e6fb39c2f8e90a55f12bb19e982db`；错误目录实现 `59db343fe6c6b0b5064057ebffa20a7c0bc7272a`，
最后提交仅整理 SDK CMake 属性排版。前置 P06 验收 `8bc364902`。
本文件是独立验收提交，不继续改变生产实现。按用户授权直接进入后续 LR00，不等待阶段复审。

## 责任与删除

| 唯一提供组件 | 公开头与注册函数 | 描述数量 |
|---|---|---|
| lux_editor_project | ProjectErrors / registerProjectErrors | 13 |
| lux_editor_context | ContextErrors / registerContextErrors | 25 |
| lux_editor_ui | EditorUiErrors / registerEditorUiErrors | 8 |
| lux_editor_app | AppErrors / registerAppErrors | 17 |

删除 FrameworkErrors.hpp、FrameworkErrors.cpp 和 registerFrameworkErrors，未增加聚合转发。
原 63 个 descriptor、canonical name、ErrorId、消息、参数、恢复分类逐项 token 对比一致。
各组件只注册所属描述；完整 host 显式装配四个目录。Project 不再为错误目录依赖 Context。
Process 错误的现有 App 边界转换本阶段未改，后续 MA02 按独立规范迁回 Process。

Source/provider 检查使用真实 CMake File API。独立 Project/Context/UI/App 安装消费者分别只请求
对应组件，验证注册、幂等、碰撞拒绝与其他目录未被隐式注册。Context 原冲突测试改为所属的
recursive-service descriptor；拒绝构造及错误参数断言保留。

人工复核及 token 对比确认，旧生产逻辑除三个装配点的注册调用外无改变；没有混入生命周期重构。
头文件自包含遗漏补在 ProjectPreparation.hpp。函数间空行及 CMake 分组按用户要求整理，
另人工修正 formatter 产生的 lambda 捕获断行与 CMake 属性名/值拆散。

## 固定提交上的实际验证

独立 clean tracked 检出及 ValidateTrackedSnapshot 通过。本次使用已有独立构建树增量验证，
不宣称冷构建；SDK 前缀初始为空，最终排版提交重新安装并完整复验。

| 范围 | 结果 |
|---|---|
| Windows Editor | all -j 4 -- -k 0，第二轮 no work，70/70 CTest |
| Windows PLAYER | all、no work，40/40 CTest |
| 原安装 SDK | 16/16；独立公共头 C++20 / 无 RTTI 编译 |
| 四目录最小 SDK 消费者 | 各 2/2，包含独立进程中的真实注册冲突 |
| 原最小消费者 | app、Project、Scene、services/tasks、TaskScope、ObjectScheduler 各 1/1；Object 2/2 |
| 真实依赖负例 | Project→Context、AppErrors 错误 provider、Context→UI、UI→Editor；逐项拒绝、去边恢复 |
| 实际 UI/文件 | UI GPU、项目替换、迟到完成、在途关闭；安装产品中文路径创建、重开与 WM_CLOSE |
| 删除和闭包 | 原聚合头/符号无活动消费者或安装残留；无 legacy、源码私有头或旧 SDK 补全 |
| 证据验证 | 搬迁到中文/空格路径通过；删除、篡改真实 SDK 日志均拒绝 |

原 62 项行为均保留；新增 8 项目录行为，不用总数代替断言/生产者核验。
首次构建暴露的 FrameworkResult 间接 include，以及首次碰撞夹具遗漏参数占位符的失败全部保留。
后者修改夹具保留原消息占位符，再触发真正的 DEFINITION_MISMATCH，未放宽生产注册规则。

## 覆盖边界与保护

本阶段未改 Object、Process、Runtime、GPU 或代码 pin 算法。P06 ASan 成绩仅按
`3f3060d6b344d480844028dbbe7d97156016db94` 继承，不宣称在 P07 重跑。
UBSan 仍为 NOT_QUALIFIED；Q-P06-CLANG-GET_DELETER 保留到 LR/MA 审计，不因错误目录测试通过而关闭。
原生输入 NOT_RUN_USER_DEFERRED；Linux、IME、历史性能保持原记录。LR08/MA11 的新必测要求另行执行。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。
Context/Pane 用户排版差异保持未提交；ProjectBuilder 补丁独立、未应用。main 与历史快照不变。
SDK：`E:/SyncForder/CodeRepos/install/Framework-terminal-p07`。
lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。

证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p07/verified-evidence/`。
共 1087 文件、160 条真实命令；manifest SHA256：
`d6e5712259c4ca6cbf4e4538d84432d891432fc68220ef8be98cc7bca7b1c248`。
原始输出在源码树外，按记录中的实际 SHA 核验；施工状态仅在 `.internal/editor-redesign/`。
