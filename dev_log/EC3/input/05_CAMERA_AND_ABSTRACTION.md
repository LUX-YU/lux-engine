# EC3-D：相机、ECS 与过度封装核查

## D1. 本轮结论的边界

已确认：CameraMotion 是导航增量；CameraPose 组合原 Transform3D 与 Camera；ViewportPresentation 把它们通过 patch 写入 ECS；CameraExtraction 读取 Camera + WorldTransform3D 并构造原渲染输入。[S26–S29]

因此“ECS→渲染成立”不能证明中间没有任何冗余；但也不能由类型较多直接证明它们都是冗余。必须逐边检查真实责任、状态和失效边界。

本章要求完成定向审核与有证据的删除，不要求推翻既有相机、Runtime、Renderer 或显示输出协议。

## D2. 明确保留的角色

| 角色 | 唯一职责 | 不能变成什么 |
|---|---|---|
| CameraMotion | 一次或一帧内合并的导航意图 | 第二份活相机组件 |
| 原 CameraPose | 导航／恢复需要的相机状态值 | Renderer 直接读取的 Editor 私有权威 |
| navigateCamera | 处理原 Camera/Transform 的纯算法 | 读文件、访问 Host 或建立资源的服务 |
| SceneView / MaterialView | 本视图输入、目标绑定和状态 | 第二个 SceneRuntime 驱动者 |
| ViewportElement | 显示图像和捕获 UI 输入 | 决定场景资产或作者历史 |
| ViewportPresentation | 本视口请求实体、相机归属、输出引用与关闭 | 新 Renderer / 通用 Service locator |
| CameraExtraction | ECS 观察和渲染操作准备 | Editor 相机控制器 |
| 原 WorldTransform 更新 | 从局部／层级计算世界变换 | 第二份 Editor 私有 transform 算法 |

原 CameraPose 改名为 `ViewportCameraState`：字段仍是原 Transform3D 与 Camera，不增加平行数据。相关 navigate result、view state codec、capture/restore 和测试一次迁完；原逻辑头可保留，旧类型名不留 alias。磁盘字段与版本不因 C++ 正名自动改动。

CameraMotion 的名称和职责已合适，除非查出实际语义不一致，不需再造 Input/Request/Command 三层包装。

## D3. 相机使用模式

至少区分：编辑器自由相机、借用内容／运行相机。原 ViewportPresentation 两种 create 入口继续复用。

自由相机：其 ECS Entity 由视口请求负责创建与退休；导航只写这一实体；不进入作者 History，不保存成游戏内容。

借用相机：只引用明确的场景实体；ViewportPresentation 不因关闭而删除它。原 setCameraPose 不应偷偷写入外部相机。跟随／驾驶若要提供，是显式模式和权限，不是看到 Entity 就默认能写。

本阶段不要求新建完整相机选择 UI，但必须保留低层借用模式回归，并明确 Run 自由观察不等于游戏当前激活相机画面。

“激活”按当前每个 RenderViewAssociation 指定相机。不得退回全局单一 ActiveCamera 导致多个窗口争夺状态。

## D4. 建立一张实际调用／拥有清单

逐条记录下面链路：

```text
输入事件 → CameraMotion 合并 → navigateCamera
    → View 的期望状态 → ViewportPresentation
    → ECS patch → WorldTransform → CameraExtraction
    → RenderViewRequest/association → 原输出 → UI 采样
```

每个函数记录：调用频次、持有数据、拥有资源、分配／复制、是否有外部 callback、是否跨帧、是否改变身份、是否需要错误转换。

判定为纯转发且同时不改变抽象边界／访问资格／寿命／错误域／线程的，合并进调用者或准确 provider；迁移所有消费者并删原体。名字短、函数短或者容易内联不是单独的删除理由。

不允许为“层数少”让 UI 直接调用 RenderRuntime 的私有资源构建，不允许让 Renderer include Editor。保留使用基础设施所需的真实边界。

## D5. 期望、ECS 当前值与已显示图像

可以有不同阶段的值，但必须有清楚关系：

- 视图的持久／期望状态用于导航、窗口重建和布局恢复。
- ECS 当前相机是运行侧被抽取的事实。
- accepted/published 输出属于某个明确的 view revision、render sequence 和相机观察。

若相机期望未变，不应每帧重新编码／patch 两份组件。利用原 pending/revision，不另加一个不同步的 dirty 管理器。

如果原接口已经保证当前提交与可见图像对应，记录证明路径即可。否则，在既有 ViewportPresentation／输出观察中携带最小的相机／输出对应信息，或在不同步期间明确推迟拾取，不能默认拿最新期望解释旧图。

这不是要求保留全世界历史帧、实现新 GPU picking 或复制每帧 Registry。范围是相机姿态、输出范围、目标实例／代次的准确配对。对持续变化的 Run 几何，明确拾取采用当前仿真还是固定显示几何的已有语义，不能暗中承诺精确历史几何。

当前报告只定位了潜在失配关系，没有复现已有 bug。先用受控输出延迟与借用相机写入测试观察；如已满足要求则保留，不为制造整改成果强行加缓存。

## D6. 拾取与拖放

拾取、模型放置和 cameraRay 必须消费同一选定相机状态及 viewport extent。DPI 只在输入坐标转换的一处处理，不能在 cameraRay 内再次缩放。

输出未就绪、旧实例已退休或 view generation 已改变时，拒绝／延后原输入，不能把点击重定向到后来复用的窗口。

模型放置的作者来源戳、目标分区和 AssetReference 验证仍保留。相机优化不能将内容多分区限制悄悄放宽，也不能把缺 MeshQuery 解释成错误的默认命中。

Scene 的工作平面属于 Scene tool；通用 viewport 不需要因此认识 ModelAsset 或项目目录。

## D7. 观察者与更新顺序

必须同时验证：系统连接前已有 Camera／WorldTransform 仍进入首次抽取；运行期间 emplace/patch/erase/destroy 正确记录变化；重绑定和窗口重开不留旧关联。[S02]

所有被观察修改继续用 patch/replace。on_construct/on_destroy 只记录必要意图／句柄，不能在信号派发中销毁正在变化的实体或同池内容。延迟命令在原唯一安全点应用，排空中新加入的留到后批。

不要把异步资源就绪改为只依赖 on_construct。回执到达后仍需要原维护路径安装和确认，不能因“观察者更纯”而漏掉异步完成。

关闭需要保留输出引用、已提交渲染资源和原 retirement。减少封装不意味着取消 output evidence、surface generation、view revision、scene domain 或 frame serial 的校验。

## D8. 可以精简的检查与不能合并的检查

| 情况 | 处理 |
|---|---|
| 同一同步算法连续两次验证同一不可变参数，无回调 | 合并为一次，传真实值／引用，不重新查 ID |
| View 创建与之后运行帧分别验证相机实体 | 有不同有效期，保留 |
| 组件 patch 之后在 CameraExtraction 处理世界变换 | 输入已改变，不把前一次校验当永久许可 |
| output status、view sequence、GPU 可采样证据 | 是不同事实，不能因为字段多就删除 |
| 多次局部 projection 计算确认完全同参数同有效期 | 复用结果，记录新的生存范围 |
| 每次导航包装同样的 result/expected 但无额外语义 | 可合并薄转发，保留准确错误域 |
| borrowed camera 的非法父级／变换 | 不允许从本地 CameraPose 假定世界变换合法 |

删除每处校验都要写“首次证明点、使用点、失效事件、是否跨回调、替代约束、实际回归”。不新增可长期保存的 Unsafe/Validated 访问去绕过未来准入。

## D9. 回归范围

同源双视口各自导航／尺寸／高亮；关闭一窗另一窗存活；作者 current/dirty/history 不变；借用相机被游戏侧 patch 后原 ECS 抽取更新；系统晚连接折入存量；延迟输出时拾取不误用另一姿态；非法相机／旧代次准确拒绝；资源失败保留最后合法状态；正常与失败关闭不泄漏原引用。

旧 EC2 原生输入延期不因本章自动恢复。本章若实际修改输入坐标／导航，必须做新增针对该路径的验证并如实标记范围，不宣称完成了原来整套 OS 输入资格。
