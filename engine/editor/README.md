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
  └── 前端与窗口
```

Editor 是业务入口，不必 is-a Window。窗口与 UI 是前端，未来开放其他前端不要求把全部编辑能力绑定到原生窗口。

具体业务由 XxxEditor 拥有：内容、历史、请求及生命周期。顶层 Editor 不写识别 Material／FlowForge 的业务分支，也不通过一个完整共享 Impl 间接访问所有业务。

开放文档类型通过既有注册和有限接口扩展，不把未来所有编辑器塞进固定 variant。固定内部状态与有限投影类型则可以采用值类型、concept 或 variant。

## 当前模块归属

| 目录 | 职责 |
| --- | --- |
| `core` | Editor、Project、通用文档及请求协调 |
| `editing` | 内容历史和通用 Undo／Redo 协议 |
| `editors/scene` | SceneEditor 业务、对应 Pane、typed Inspector 生成链 |
| `editors` 中其他具体包 | 各自业务与 UI 接线 |
| `ui` | 通用前端、布局与窗口协作 |
| `rendering` | EditorRenderer、RenderView、UI 帧和图像引用生命周期 |

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

## 当前与待接线能力

当前具有生成式 Inspector、Scene／Run 的 Main 推进及同视口切换。视口仍使用私有三维 SceneCamera，运行时目录仍存在静态身份耦合。

通用相机组件、CameraMan、基于 Entity 的完整空间查询、视口空间扩展及 Feature 自有高亮集合属于已记录的设计意图，不能通过文档新增就登记为产品已实现。

相关说明：[渲染前端](rendering/README.md)、[Scene](../scene/README.md)、[Process](../process/README.md)。
