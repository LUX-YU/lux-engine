# L3：工作台收拢，具体工具消费作者与活动，不再拥有它们

前置：L2 活动已独立。这里迁移的是已实现正式新 UI 和 interaction，不重写 ImGui/node-editor，不进行 P12 产品入口切换。

## 1. 物理归属和构建边界

| 原位置 | 新位置 | target 处理 |
|---|---|---|
| desktop | workbench/desktop | 保留 ViewHost/desktop shell 实际 CPU/GPU 边界 |
| views/api | workbench/desktop 的公开 View 契约 | 保留现有 view_api，只有一个真实头集合 |
| views/viewport | workbench/viewport | 保留 editor_viewport；不引入作者 Session |
| widgets | workbench/widgets | 原 GraphCanvas/NodeCanvas/TreeRows 单一实现 |
| tools/scene/interaction | workbench/scene 的 interaction 文件组 | scene_interaction 仍 CPU |
| tools/material/interaction | workbench/material 的 interaction 文件组 | material_interaction 仍 CPU |
| tools/flowforge/interaction | workbench/flow 的 interaction 文件组 | flow_interaction 实际名称以 L0 为准，仍 CPU |
| tools/*/ui | 对应 workbench/领域 | UI 与 interaction 不混成一个 target |
| project/ui | workbench/project | 只迁 ProjectView/AssetPicker，不携带 CatalogModel |
| tasks/ui 的剩余 TaskView | workbench/tasks | tasks_ui 消费 E2 Monitor |
| tools/settings 的真实纯 UI | workbench/settings | Context/插件安装接线不混入控件库 |
| editing/sinclude 的 InteractionDelivery | workbench/sinclude 的局部共有算法 | 两工具 PRIVATE 可见，不安装或新增 Manager |

同领域中少量协作文件优先 `include/src/test` 内归组；保留 interaction/ 子目录是实现选择，不要求每一小组有自己的 include/src/CMake。

## 2. Desktop 与 View API

### 2.1 保留的公共能力

IViewHost/DetachedView/ViewInfo/ViewCloseResult 使用 L1 确认的唯一类型。ViewHost 继续独占完整 DetachedView；Root 只登记对象树；DesktopShell 借用原 Process/Renderer，不拥有 Session/Run/Save 的 authority。

### 2.2 特殊成员和销毁顺序

- DetachedView 和一次性挂载准备只能移动，不能复制。
- 移动赋值先组成旧拥有单元再释放旧 Pane/code，不能先换旧 code 再调用旧 Pane 析构。
- Activity Operation 的不可复制/不可按值移动约束继续保留；只有外层 unique_ptr 可以转移。
- 对象在自身线程销毁；不能在信号回调栈中物理删除自身或祖先。
- 外部持有 token 销毁时，内部提交的局部记录仍活到通知返回。

### 2.3 关闭结果不可再粗暴压成 BUSY

临时 Busy 保持同一关闭记录等待；永久失败记录原 domain/code/message 且不无界自动重试；已挂载事实不因通知失败改写为未挂载。Host 只使用准确关闭结果，不知道所有领域 error variant。

迁移中错误头拆分只影响 include/provider，不改变关闭协议或生命周期算法。

## 3. 通用 viewport 与 widgets

`ViewportPresentation/ViewportElement/CameraNavigation/相机输入` 继续共享一份通用实现。作者放置/模型创建等 Scene-only 计算留 workbench/scene 或相应领域，不为 Material 使用 viewport 引入它们。

GraphCanvas 继续调用用户既定的 ax::NodeEditor；它只处理 CanvasNode/Pin/Link、屏幕交互和本地 ID，不拥有领域图/History/ContentStamp。将 UI 事件转成领域 edits 的位置是具体 View。

P10Q 已建立的画布身份安全整理保持：
- 只在安全点重建；有在途编辑不非法整理；
- 保留真实作者布局、pan/zoom/selection；
- 不复用旧 UI 身份去命中新作者对象；
- 不访问第三方私有实现头；
- 不因为目录迁移扩大无界保留表。

## 4. 具体工具 UI 的三个数据状态

每个 View 只允许拥有语义不同的三类内容投影：
1. display 的只读已提交快照及 stamp；
2. `NodePropertiesDraft{based_on,value}` 等可取消本地草稿；
3. 有界排队输入 `{payload,based_on,phase}`。

它们都不是新的作者 authority。普通刷新不能覆盖草稿来源；BEGIN/PREVIEW/COMMIT 的校验保持 P10 R1 规则；BUSY 不重新取 current 或重新 begin；STALE 不每帧自动 rebase。

### 具体需删除/避免的成员

- 若原 `edits_`、单独 `properties_base_` 或 `draft_base_` 已在 P10 R1 删除，不得为了新模板方便恢复。
- View 不恢复 `compile_task_ / compilation_result_ / save_pending_ / run_instance_` 等已迁出业务 owner。
- 允许必要的显示状态、已看到结果 ID、错误呈现；它们不能成为第二个 service state。
- 关闭一个 View 不停止仍由应用活动持有的保存/编译/Run。

## 5. CPU interaction 在 E3 的规则

物理位置属于工作台，不增加 ImGui/Pane/Root/Vulkan 依赖。它们继续只依赖领域模型、原运行 inspect 需要的纯契约和 edit gate。

选择同步：同完整内容戳下复用已验证身份存在事实，但每次仍经过 Store 访问和 gate；内容变化、重载、关闭、代际变化失效。不能把 selection_source_ 放宽成 HistoryId 或 bool initialized。

临时 BUSY 或错误线程不能触发旧身份清理；只有明确 STALE_SESSION 走失效路径。析构可能执行插件代码的 payload 仍在恰当的读取准入内清理。

## 6. 属性生成器、Inspector 与扩展 UI

- 生成器脚本、模板、support 与对应消费目标一起迁至 workbench/scene，不能只动 C++ 文件。
- 生成输入路径从原源目录重定位到新真实目录；保持 schema、生成符号和实际数据语义。
- 生成头不是另一个源权威；删除旧输出后应能从正确依赖重建。
- 如果代码生成需要编译历史路径，更新实际 generator 工作目录/命令，不建立旧路径软链或全局 include。
- ComponentEditorRegistry 的 UI 工厂语义归 E3；纯配置值/schema 归 E1/现有 modules；plugin 贡献安装归 E4。P11 尚未实现的动态注册不在此时伪造。

## 7. Tests 不反向污染库

新的跨 Material/Flow/Scene/Host/GPU 测试代码放 `editor/tests/integration/`，或先将测试定义延后到该入口。所属文件可为领域 fixture，但 target 必须在所需生产库配置完后创建。

例如原 scene/projection 测试使用 editor_viewport：这是测试组合 E2+E3；不要把 editor_viewport 加到 scene_projection 的 PUBLIC_LINK_LIBRARIES。依赖检查应区分 test 与 production 源角色。

`cmake/installed-consumers` 可保持原位置，因其承担全仓安装测试，不为了根图统一再移动无关目录。只更新实际依赖、源码定位、模式和新安装入口。

## 8. L3 验收

- 真正的三个 interaction CPU consumer 可独立编译/运行；不借 ImGui/Root 的偶然传递头。
- widgets/viewport 不包含具体源模型；Material 不链接 Scene UI。
- TaskView 和 ProjectView 读取唯一提供者，关闭视图不破坏 Model/Monitor/任务。
- 真实 Root 的离树、挂载、通知、卸载、重绑定失败与全局用户改动保全通过。
- 新 SceneView 双视口 GPU/原生输入、Material/Flow 正式属性/画布来源路径按影响运行，不以旧窗口或手工模型 API 代替。
- 没有重复的根 UI/通用 viewport 代码；旧纯算法原体退出，必要旧转换仅限原消费者。
