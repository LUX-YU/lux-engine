# ER-2：有限场景编辑续行（2026-09-12）

用户本轮明确授权“改进后进入 ER-2”。保留既有 Application / Window / Session / Workspace / Pane /
Renderer 分工；不进入 ER-3，不合并 main，不升级依赖。开发基线为 ff31bcb1，资产路径显示修正已单独提交。
本报告在最终资格后补齐候选身份与结果；下列开发结果不替代最终安装资格。

## 实现与所有权

- `SceneEditInput` 由冷 source factory 提供稳定 WorldObjectId、实际 Entity 和有限 Transform3D / Light3D
  作者初值。工厂核对 Transform schema v1 / Light schema v2、实际组件和值；失败保留原 Scene owner 和输入数组。没有复制 WorldDescription。
- `SceneSession` 唯一拥有作者状态、预览和既有 EditHistory。一个 Session 同时接纳一个属性手势；Pane
  持有该手势令牌，不拥有内容或历史。另一个 Session 的身份或另一个属性种类不能使用此令牌。
- Operation 拥有单个对象的前后值和原 StateId；prepare 核对当前作者值，计入 staging 预算并准备一份替换值。
  apply 仅交换预备 unique_ptr，标记待同步。Registry、GPU、通知和可失败分配均不在 apply 中。
- 作者与 History 提交后发布内容通知；Scene 投影在 owner 安全点执行真实 patch，已有 TransformSystem
  负责派生 WorldTransform。投影失败保留作者和历史，通过 `projectionFailure()` 暴露，后续安全点重试。
- 有效 preview 更新独立预览版本；非法 preview 保留最后合法值；提交失败保留手势与待提交 operation。
  净零提交不截断 redo。Esc、失焦、选择切换、隐藏/关闭 Inspector 取消预览，不创建历史。
- Inspector 显示资产挂载路径；优先显示 Transform/Light。工具栏“Reset selected transform”是第二个真实
  场景输入入口，同样经 Session/History。局部 Undo 保持原 Session；窗口历史菜单仍在打开时固定目标。
- 没有正式 codec，当前仅支持内存编辑。窗口关闭提供继续编辑 / 放弃关闭，不伪造保存成功或 clean。
  显式宿主 requestClose 和有限帧测试退出按调用方的明确关闭请求处理。

## 正式入口与存量消费者

正式 `lux_editor` 由新的 `editor_application` 产出，默认打开有限编辑 Scene；`--inspect` 显式只读。
原 Workbench/混合 Presenter 移至 `engine/editor/test/legacy_scene` 和 `legacy_application`，仅 BUILD_TESTING
创建 TEST 目标，保留已有回归与同量成本基线。旧正式启动代码删除，旧 Scene 包不再安装。

架构门禁对这两个明确的测试目标及唯一历史成本消费者登记窄例外；生产 EDITOR 仍禁止链接 TEST，
新 Application/Session/UI 的源依赖扫描保留。未迁的材质/FlowForge 及其旧通用 Inspector 消费者仍独立保留；
这不是 ER-3 迁移，也没有新栈调用旧 Journal 或可写 Registry 绘制 fallback。

## 开发验收映射

| 条目 | 实际覆盖与当前结果 |
|---|---|
| E01 只读 | 原 SceneSession 协议测试，内容方法 READ_ONLY / 历史 BLOCKED，通过。 |
| E02 单属性 | Transform/Light 作者值、真实 Registry patch；七张 GPU 回放截图，修改有差异、恢复逐像素相等，通过。 |
| E03 prepare 失败 | source 值不匹配、wrong Session/thread、staging 限额，通过；15 个工厂分配点、3 个提交分配点在 MSVC 专用 DLL 诊断中逐点通过，失败后同令牌/operation 重试。 |
| E04 no-op | undo 后净零提交，current/revision/cursor 与 redo 保留，通过。 |
| E05 手势 | 100 次 preview → 一条历史；提交不重复叠加已经预览的增量，通过。 |
| E06 取消 | Session 选择切换；真实 Inspector 的 Esc、窗口失焦、隐藏；保留作者/history/redo，通过。 |
| E07 非法值 | NaN 拒绝且保留最后合法 preview；Transform/Light 令牌不能交叉提交，通过。 |
| E08 两入口 | UiInputEvent 驱动真实 Inspector 和工具栏按钮交替修改；两步 Undo 顺序正确，隐藏/重开后可回放，通过。 |
| E09 通知失败 | 实际关闭 queued receiver 的队列，通知连接断开、未交付，内容已提交且同令牌不能重复提交，通过。 |
| E10 保存版本 | 无正式 Scene 作者 codec，不开放保存；本阶段只声明内存编辑，未作持久化验收。既有 EditHistory 票据机制不改写。 |
| E11 投影失败 | 专用诊断将实际 Transform 组件移除，验证准确 STALE_ENTITY/session/subject 与恢复重试，MSVC 诊断通过。 |
| E12 旧写路 | 正式入口切换，Scene UI 只读值/typed Session 意图；旧正式 Scene 库退出 SDK。待最终实际依赖闭包确认。 |

UiInputEvent 回归是辅助自动化输入测试，不改标为原生 Windows IME 验收。
用户此前已确认真实 IME、鼠标捕获、中键平移和右键旋转正常，见 ER-1 人工验收补记；不重复否认该结果。
本轮新增属性输入及关闭确认的完整人工桌面体验未单独获得用户复测反馈。

## 原始证据与资格约束

开发原始输出位于本轮 evidence/logs；保留失败记录：输入驱动未命中字段、连续点击进入文本模式，
首次诊断配置误选 Clang，以及新增 schema 校验误用 Light v1 后按实际 v2 修正。前两类驱动/工具配置问题不记为产品缺陷；Light 版本接纳失败按本轮实际修复记录。失败日志不改为通过。
成功的辅助 GUI 输入运行包括 `er2-normal-gpu-candidate.log` 与 `er2-diagnostic-gpu-editing-03.log`，包含七状态 GPU 回放、取消、隐藏后回放和完整正常关闭。

普通构建与专用故障 DLL 分离。最终正常 SDK 必须来自 tracked 候选 clean clone，执行 all/no-op、CTest、
实际 GPU、原前缀与迁移后安装消费者，再记录源码 Git blob 与 checkout 换行后 SHA256。旧 q7/q8/ER-1 日志保留原身份。

有限成本只测固定单对象、100 次预览/手势、完整 execute/undo/redo 与实际 headless Scene 投影和关闭；
1,000 / 10,000 手势各五个独立进程，独立 100 次 warmup。该结果不声称 GPU 成本、逐操作 p95 或相对旧栈加速。

## 当前已完成的构建验证

MSVC RelWithDebInfo 专用诊断 all/no-op 通过；CTest 157/157 通过，包含恢复后的历史回归与新编辑协议。
除编辑七状态 GPU 回放和实际 Pane 输入外，17 个诊断 GPU 回归通过，覆盖真实分配/构造失败、View 接纳、
Workspace 原失败、资源发布/快照、独立 material/shader 子失败、背压、shared GPU、输入与关闭。
普通开发构建 all/no-op、受影响 Scene 测试、编辑 GPU 路径及一次 1,000 手势成本预检通过。
这些是开发验证；最终 clean clone / SDK / 五组正式成本结果由交付 manifest 单独绑定候选。

最终候选和安装资格结果见同目录 QUALIFICATION.zh-CN.md；本文件的开发结果保留原阶段含义。
