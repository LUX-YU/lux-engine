# Scene Editor：空间交互、相机与内容历史

SceneEditor 拥有场景编辑的业务内容、作者历史、运行上下文与已接纳请求；Pane 拥有局部交互。本文描述长期职责与当前支持范围。

## 当前归属

| 路径 | 职责 |
| --- | --- |
| `include/lux/engine/editor/scene` | SceneEditor、编辑协议及公开业务接口 |
| `pinclude/lux/engine/editor/scene/detail` | 私有业务状态与算法 |
| `pinclude/lux/engine/editor/gui/scene` | 对应 Pane 和局部 UI 类型 |
| `src` | 业务实现 |
| `src/ui` | UI 接线与交互实现 |
| `codegen`、`cmake` | 组件 ImGui 生成链 |
| `test` | 业务、生命周期与相关集成验证 |

`editor_scene` 编入业务；`editor_scene_ui` 编入 Pane 及生成式 Inspector。两者都不是游戏需要依赖的模块。

## 唯一内容与身份

Registry 保存当前组件值，SceneEditor 的历史记录内容变化。运行时查询与选择面向所属 Registry 的完整 Entity；WorldObjectId 只在持久内容、保存恢复和需要的映射边界使用。

SceneObjectRow、SelectionNotice、Inspector 与字段写入目标使用 `SceneEntityRef { SceneInstanceId, Entity }`。每次运行有新实例身份；删除并 Undo 恢复的 Entity 具有新代次，旧输入被拒绝。作者历史内部通过持久映射找到恢复后的 Entity；运行对象不补 UUID。

选择从同步查询取得 Entity 后，应验证它仍属于原检查目标且代次有效。异步操作还要验证文档、作者／Run 目标代次和请求顺序。后来一次 Outliner 选择不能被迟到拾取覆盖。

空白命中可以清除选择；失效、失败或目标已切换不能伪装成空白命中。选择不新增内容历史，也不使作者文件变脏。

## 相机与空间视口

`SpatialViewport` 是冷注册的开放接口；首个实现 `SpatialViewport3D` 支持透视和正交。ScenePane 不保存位置或投影副本。尚未提供二维实现，不兼容的 World 显示限制。

对象关系如下：

```text
ScenePane：外观、输入归属、图像展示
  → 与 World 能力兼容的具体空间视口
      → CameraMan Entity 的 Camera／Transform3D 组件
      → 对应运行时空间查询
```

三维视口可以提供透视和正交导航；二维视口解释二维平移、缩放和坐标查询。未来空间插件通过窄的开放接口注册，不能用一个封闭枚举穷举全部世界类型。

WorldDescription 的 schema 和空间能力约束兼容实现，但不唯一决定投影。多种能力匹配时由视口配置明确选择；不兼容时显示准确限制，不默默套三维透视相机。

## CameraMan

CameraMan 是编辑器 owner 创建的真实 Entity，带通用 Transform 和 Camera 组件。位置和投影参数只保存一份，导航直接修改组件并通知变化。

它不是可持久化的游戏对象：

1. 不进入作者 WorldObjectId 表和正式作者对象集合。
2. 不进入保存、Run 捕获、游戏导出或内容 Undo。
3. 用户的删除、子树删除和普通业务操作不能删除它。
4. 若显示在 Outliner，归于编辑器对象区域并标识锁定。
5. 视角不写入游戏场景源；当前首次打开采用固定作者视角，不承诺跨进程保存导航位置。
6. 关闭视口／文档时，实际 owner 在解除渲染借用后正常销毁实体。

可以在实际作者 Registry 中建立该实体以复用派生和相机提取，但保存与 Run 捕获必须以正式作者集合为准，不能复制全部 Registry。

CameraMan 的“不可删除”是业务权限约束，不是永远泄漏的实体。对象关闭顺序仍需满足 Registry、View 和 GPU 资源的寿命。

## 编辑与 Play

默认同一 ScenePane 完成切换：

```text
编辑：CameraMan 观察作者世界
Play：用户 Camera 观察独立运行世界
Stop：恢复作者世界、CameraMan 和编辑选择
```

运行世界没有有效用户 Camera 时，场景区清为黑色并显示编辑器提示；不能把 CameraMan 静默变成游戏默认相机。自由观察运行世界若要提供，应是显式调试模式。

没有 RenderSystem 的场景仍可打开，编辑器 UI 不消失；ScenePane 说明没有场景渲染能力，不私自修改用户的 SceneDescription。

作者与 Run 隔离并由 Main 推进；Play 使用唯一 primary Camera，零个或多个均明确提示并清除输出。通过“Create game camera”把当前视角创建为普通作者实体，产生一条内容历史。

## 拾取链路

```text
鼠标在图像内容区域的位置
  → 当前相机投影与姿态生成射线／二维坐标
  → 查询当前活动实体
  → Entity
  → SceneEditor 更新选择
  → LuxObject 通知 Inspector 与 Outliner
```

坐标转换使用实际内容区域和一致的 DPI／UV 约定。中键平移、右键旋转、文本输入、弹窗和资产拖放不能误触发左键选择。

查询不需要 World 分区索引，不读取未加载分区，不将 Entity 转换成静态 ID 才返回。普通 CPU 几何查询不依赖 GPU readback；需要精确像素拾取时另选明确的渲染能力。

空间查询只返回结果，编辑器选择、通知及历史归 SceneEditor。查询不能递归修改或销毁正在执行的业务对象。

## 高亮

高亮是选择结果的一种视觉使用方式，并不属于拾取算法。

编辑器将选择解析为当前渲染对象集合，在正常推进点通过 Highlight Feature 的协议提交；RenderSystem 不增加 `setHighlighted()`，作者 Mesh／Material 不增加编辑器选择字段。

Feature 拥有效果状态，完整代次防止高亮复用后的对象。支持层级时可以解析选中节点的可渲染后代；非层级 World 不强行引入父子关系。集合在选择或结构变化时更新，不每帧重建全部目录。

## 从模型资产创建实体

ResourcePane 传递 AssetReference；SceneEditor 的 `requestModelCreation` 使用 Process 读取模型并转换成实体，模型不是 Scene 概念。

```text
资产拖放 → 具体空间视口解释落点
  → 固定作者目标、资产、坐标、分区与内容版本
  → Process 读取／解码
  → SceneEditor 接纳创建
  → 一条历史
```

三维落点优先使用 Mesh 表面；真正未命中才与显示的工作平面相交，默认 Y=0，可调整。查询未就绪等待原请求，业务失败保留原因，不冒充空白。不得在无有效落点时静默放到相机前方任意距离。二维与其他空间由相应视口解释，不能都传入写死的 Y=0 规则。

加载期间用户进入 Run，结果也不能被写入运行世界。原 Pane 销毁后，已接纳请求仍由文档 owner 推进。失败保留准确原因，重试使用明确目标与版本。

整次创建是一条内容操作，Ctrl+Z 撤销全部实体。单 Mesh 直接挂在对应节点 Entity 的 Mesh3D 上，Mesh3D 已持有材质引用。支持 Parent 时保留模型层级；否则累积变换展开。剪切、奇异变换、未支持变形明确拒绝。Prefab／实体装配资产不属于当前实现。

## 字段编辑与历史

生成的 typed ImGui 直接编辑有效组件。首次写入前保留 before，每次变化发布脏通知，结束时取得 after 并登记已经发生的修改，不能再次执行一次修改。

普通字段不维护通用 Draft／preview.after 副本；四元数欧拉显示和未完成文本可以有控件局部表示。容器结构变更不能跨失效后继续使用旧指针。

历史接纳失败保留未完成记录；选择切换、保存和关闭需正确收尾。完成编辑后支持 Ctrl+Z／Redo，不重新引入已取消的持续按住鼠标 Esc 回滚或裁剪特殊手势要求。

局部 Undo 固定原 SceneEditor 及其检查目标；EMPTY／BUSY／CLOSED 不回退到其他文档。Run 暂停编辑与作者历史分开。

## 通知、关闭与输入

低频业务变化继续使用 LuxObject。旧 Pane 退出时解除连接；重新建立 Pane 读取当前业务值，不能重复订阅。隐藏、销毁和文档关闭分别推进。

本帧已经绘制的图像引用按实际使用保留，不能因随后隐藏 Pane 而从封包中遗漏。关闭未完成保留 owner，不在通知栈中直接删除。

文字输入、IME 和局部文本 Undo 遵守现有 UI 输入归属，不穿透到另一个文档历史。真实输入与接口注入的验证范围不能混用。

相关说明：[Editor](../../README.md)、[ECS](../../../domain/simulation/ecs/README.md)、[空间查询](../../../domain/spatial/README.md)、[Editor Rendering](../../rendering/README.md)。
