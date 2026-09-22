# Editor：业务入口与编辑器边界

本目录保存长期有效的模块设计说明。按日期或阶段命名的实施报告、交审记录、日志、性能样本、截图归档和临时操作清单不放入产品源码树；既有历史可通过 Git 查询。

本文记录设计责任，不作为任何阶段“已经验收通过”的声明。

## 顶层结构

```text
Editor
  ├── 项目配置、文档注册与打开、布局恢复、主循环
  ├── 具体 DocumentEditor
  │   ├── SceneEditor
  │   ├── MaterialEditor
  │   ├── FlowForgeEditor
  │   └── 可扩展的其他编辑器
  ├── Editor SceneInstance → UIRenderSystem → UiRenderFeature
  └── 原生窗口、共享 RenderRuntime 与 Process
```

Editor 是业务入口，不必 is-a Window。当前产品由 app 直接装配原生窗口和 UI Scene；具体业务与窗口依然分离，未提供无窗口 Editor 产品。

具体业务由 XxxEditor 拥有：内容、历史、请求及生命周期。顶层 Editor 不写识别 Material／FlowForge 的业务分支，也不通过一个完整共享 Impl 间接访问所有业务。

开放文档类型通过既有注册和有限接口扩展，不把未来所有编辑器塞进固定 variant。固定内部状态与有限投影类型则可以采用值类型、concept 或 variant。

## 当前模块归属

| 目录 | 职责 |
| --- | --- |
| `app` | Editor 产品入口、平台输入／IME、窗口、主循环与退出协调 |
| `core` | Project、通用文档与请求协议 |
| `editing` | 内容历史和通用 Undo／Redo 协议 |
| `editors/scene` | SceneEditor 业务、对应 Pane、typed Inspector 生成链 |
| `editors` 中其他具体包 | 各自业务与 UI 接线 |
| `ui` | UIRenderSystem、Pane 登记、布局、焦点和命令路由 |
| `modules/function/render/runtime` | 共享 RenderRuntime、View、图像引用与后端退休（位于 modules 层） |

源码按 `include/pinclude/sinclude/src` 组织；业务细分放在这些目录内部的命名空间路径，不在子模块根散落另一套 project/run/asset 等平行结构。

## 局部状态与对象关系

Pane 保存局部交互、焦点、显示状态和所持 View 的使用事实。DocumentEditor 保存业务内容与历史。Renderer 保存渲染生命周期。

固定成员优先直接持有；接口扩展、独立生命周期或地址稳定性确实需要间接所有权时才使用指针。PImpl、shared_ptr 和 noexcept 本身都不是责任分离或正确性证明。

业务变化使用 LuxObject 的既有信号与生命周期机制。图像引用、帧运输、运行观察和每次鼠标状态不因此改成信号广播，也不增加第二套 MessageQueue／EventBus。

通知回调可以请求关闭和标记失效，但物理删除发生在正常 owner 安全点。隐藏 Pane、销毁 Pane、关闭文档是三件不同的事。

## Scene 编辑的核心边界

- WorldObjectId 属于持久内容；运行时访问与空间查询使用 Entity。
- 作者 World 与 Run 可以隔离，同一个 ScenePane 切换显示。
- CameraMan 是编辑器临时实体，使用通用 Camera 组件，不导出到游戏。
- 鼠标选择、拖放放置、工作平面和内容 Undo 属于编辑器。
- 射线检测是运行时能力，供 Editor 和脚本共用。
- 高亮是 RenderFeature，选择只是它的输入来源之一。

具体规则集中在 [Scene Editor 说明](editors/scene/README.md)，避免在顶层重复维护另一套协议。

## typed Inspector

组件元信息决定字段 UI，构建时生成直接的 typed ImGui 调用，每个组件对应生成实现。生成代码只编入 Editor 的 UI target，不进入领域或游戏 DLL。

普通字段直接编辑 Registry 中的实际值，首次写入前捕获 before，结束时捕获 after，必要时登记一条历史。变化立即走组件脏通知；不维护持续同步的通用 Draft 和 preview.after 副本。

格式转换、未完成文本等控件可以有局部表示数据，但不发展成另一份权威模型。

## 推进与异步

Main 拥有 Scene 和编辑业务。Process 承担明确的有限 IO、解码和编译；结果在 owner 的正常推进点采用。Simulation 使用现有 TaskGraph。

必要 Scene 更新获得公平的提交机会，再推进前端渲染，避免 Frame 持续抢占容量导致“数值变化、画面仍是旧值”。背压保留原包，不重绘并重复执行业务。

关闭未完成保留 owner；首个原始失败与实际进度一起保留，不能因为最后资源清理成功而把失败改成成功。

## 工程约束

- 不为普通分配增加全工程 OOM 恢复机制；允许正常内存分配。
- 业务失败、失效身份、线程契约和资源关闭仍需准确表达。
- `if/for/while` 等控制体使用显式大括号，不将循环和循环体挤在一行；不同语义之间留空行。
- 不因增加一个效果就给顶层 Editor／RenderSystem 增加专用功能。
- 源码树保留模块文档、使用说明和测试；过程报告与产物不进入安装 SDK。

## 唯一推进路径

Editor 按“平台输入和完成采用 → 一次可接纳的 UI 构建 → 结束字段借用 → SceneDriver → RenderRuntime”推进。UI 背压保留同一帧，不重新调用 Pane；仍推进输入、回复和退出。作者与 Run 均使用 SceneInstance 内的一份推进记录。

回复、Program、Control、资源请求检查和新模拟步各有独立预算，同类预算在所有 owner 间共享。文档、UI Scene 和 Runtime 共同轮转起点；一个持续有工作的文档不能耗尽 UI 的发布机会，UI 也不能耗尽资源退休机会。帧仍持有捕获时的准确图像版本，Program 使用既有 FIFO 和图像依赖，不用重复 UI 构建补偿推进顺序。资源请求预算计量一次待处理 Entity 的检查及其有界依赖集合，不代表一个 GPU 命令或一次磁盘读取。

Scene 文档注册保存按 Project 实例／catalog revision 区分的弱资源来源记录。同版本文档和 Run 共用不可变读取、Mesh／Material／Texture 及查询几何；Run 保留启动来源，新的项目版本不覆盖它。Project 本身不依赖 Renderer。

当前实现包含 Camera 组件、Editor-only CameraMan Entity、Entity 空间查询、空间视口扩展及 Feature 自有高亮集合。平台／桌面组合的实际验收结果属于树外证据，不能由本设计说明推导。

相关说明：[Scene](../scene/README.md)、[Process](../process/README.md)、[渲染运行入口](../../modules/function/render/runtime/README.md)。
