# Scene 的 Render 数据接线

此模块实现 Engine ECS 数据到 Render 公共能力 Payload 的转换。Render 不读取 Registry；
专业行为不访问 Render 内部 Feature 或 Storage。应用调用 `registerBuiltinRenderBindings()`
登记内置声明，再以 `prepareRenderBindings()` 查询选中能力、解析依赖和有效 ECS 输入。
`RenderSystemDependencies` 借用准备好的计划；RenderSystem 构造时保存自己的计划值。

## 冷态与阶段

`RenderBindingPlan` 保存所选声明、实际能力身份和有效数据需求，不拥有 Registry 或 GPU 对象。
Scene Builder 在构造系统之前将动态需求与静态需求一起交给原 `SceneCompatibility` 算法。
不同 Scene 的选择不会修改共享的 System 描述。缺少依赖、版本不符及依赖循环在冷态拒绝。

RenderSystem 只负责 Render Scene 的创建/删除、行为构造、阶段遍历和帧封包边界。
它不包含 Mesh、Material、Light、Camera 或专业 Command。内置转换在此模块登记；
扩展作者可以使用 scene_render 的模板和 concept，不必链接此内置模块。

维护阶段先采用实际完成结果，再在稳定点准备帧数据，最后向现有 Main FrameInput 封包。
同步帧操作不创建 Sender；异步资源工作交给共享 ExecutionRuntime。没有新线程池、队列或 Scope。
`whenReady()` 在能力安装和 EnTT 连接完成后返回；调用者必须持有 Scene lease，直到采用或清偿。

## 声明与事件

组件关联声明 `SourceComponents`、`ResultComponents`，能力操作另声明更新策略。
`Dependencies` 声明实际行为借用；冷态按依赖排序构造，停止时反向清偿。
模板连接 EnTT `on_construct`、`on_update`、`on_destroy`，并折入连接前已有的组件。
信号直接调用类型化入口，热路径不再按组件 Hash 派发。冷态索引比较完整 TypeToken。

作者或系统写入必须使用 `registry.patch<T>()` / `replace<T>()`；直接修改 get 返回的字段
不会发出通知。观察者只记录需求、脏实体或退出句柄，不在信号栈里改组件、等待 IO 或提交资源事务。
Scene maintenance 仅在允许结构修改的安全点装入结果。结果组件不进入作者序列化或 Inspector。

## Mesh、材质与资产

`MeshRenderBinding` 处理 Mesh3D 的资产需求、候选替换及退出。已采用的 MeshId、MaterialId、
MeshInstanceId 分别在隐藏资源组件中；候选集合只包含尚未完成的需求，不复制 live 资源目录。
资产按 Pipeline、实际输入目的地、来源域和版本复用。CPU Cooked Material 与 GPU Material
使用不同表示表；后者引用已就绪的 Cooked 和 Texture，不重复读取或解码。

新候选先准备依赖，再等待有关 View 采用可用几何/材质集合，然后创建或替换实例。
完成后仍须匹配实体 generation 和当前需求代次。迟到成功不安装到过期实体；已接受的替换
必须撤回其实际引用后才能释放候选。创建失败不伪造 ready，替换失败保留旧关联。

`RasterTransformOperation` 观察 WorldTransform 和已采用实例，直接把 IDs 与矩阵写进 FrameInput。
它不持有第二份完整 Transform 表，不逐实体 spawn。帧拒绝后沿 Requester 的 resynchronization
重新提取当前权威数据。`LitSceneInputOperation` 只在输入结构 revision 变化时重新准备几何、
材质程序分组和阴影输入；内容未变不重新编译 Effect。

## Light

`DirectLightBinding` 处理 Light3D/WorldTransform 的存在性、创建、替换和退出。
`DirectLightOperation` 只处理已采用 Light 的脏参数。位置、强度和颜色更新不重建资源；
阴影描述或实际类型变化才改变冷态输入。移除后重加按需求代次区分。

## Camera 与输出

`CameraViewBinding` 拥有 CameraTargets 到 View/Effect/Attachment 的专业关系。
`CameraRenderResourceComponent` 保存已采用关系；有限候选保存尚未采用的回复。
Editor 在 Scene 就绪后通过冷态 `binding<CameraViewBinding>()` 取得借用，直到该 Scene 退休。
CameraTargets 的更改走 EnTT patch，关闭与 Resize 沿已有 Target/Scene 接口处理。

`CameraInputOperation` 为各 View 分别生成参数和 viewport 数据。地址及字节由 FrameInput 持有，
不同 View 不共享一份可变相机输入。仅消费颜色的能力不计算未使用的相机矩阵。
目前实际行为对象按 Scene 构造，Camera 行为内部管理多个 View；任意扩展声明的 VIEW 范围
尚不提供逐 View 自动构造，冷态准确拒绝，不能将其误称为已支持。

## 停止与共享引用

Scene stop 阻止新需求，继续清偿已接纳工作。依赖的反向顺序为：退出 Camera 输出，结束帧操作，
解除实例关系，再归还该 Scene 的资产引用；之后才删除 Render Scene。
连接先于行为对象析构，Registry、Requester 和共享 ExecutionRuntime 活得更久。

当前 LitScene 的材质目的地是 Global。停止 Play 只归还 Play 的引用，不能等待整张 Global
材质表清空；编辑 Scene 仍可使用它。RenderAssets 负责最后引用释放及异步逻辑释放，应用关闭
在全部 Scene 结束后清偿该设施。GPU 物理清理始终由 Render 唯一 cleanup_scope_ 完成。

模块代码必须覆盖行为、Pipeline 表及其最终回调。内置代码静态链接；外部动态 Pipeline 的独立
代码 pin 与热卸载不属于当前已验收路径。配置多个同类型资源角色时，专业结果组件也必须表达
实际角色；内置 Mesh 组件当前只表达一个已采用角色，不宣称支持同时挂多份 Mesh 表示。
