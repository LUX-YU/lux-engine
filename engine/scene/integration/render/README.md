# Scene 的 Render 数据接线

此模块实现 Engine ECS 数据到 Render 公共能力 Payload 的转换。Render 不读取 Registry；
专业行为不访问 Render 内部 Feature 或 Storage。应用调用 `registerBuiltinRenderBindings()`
登记内置声明，再以 `prepareRenderBindings()` 查询选中能力、解析依赖和有效 ECS 输入。
`RenderSystemDependencies` 借用准备好的计划；RenderSystem 构造时保存自己的计划值。

## 冷态与阶段

`RenderBindingPlan` 保存所选声明、实际能力身份和有效数据需求，不拥有 Registry 或 GPU 对象。
Scene Builder 在构造系统之前将动态需求与静态需求一起交给原 `SceneCompatibility` 算法。
`resolveRenderBindings()` 是 UI 和实际准备共用的纯冷态算法；UI 在能力回复到达时计算资格，
绘制时只显示已计算结果。登记可提供能力名称限定；没有限定时按完整 Payload 合同选择。
不再因 EFFECT 种类自动启用 Camera。对不提供 CameraInput、但产品明确需要相机输出的能力，
装配调用 registerBuiltinRenderBindings 时显式传入该能力名（包括仍需支持的其它相机能力名）。
不同 Scene 的选择不会修改共享的 System 描述。缺少依赖、版本不符及依赖循环在冷态拒绝。
Editor 的 Create 操作先准备 Plan，再调用 `SceneRuntime::Builder::validate()`；失败时不写 Scene
或项目目录。实际构造复用该 Plan 和同一个静态/动态需求解析算法，validate 本身不创建 Registry、
System、SceneId 或 GPU 对象，不承诺后续设备/文件操作一定成功。版本、配置 Schema、
实际 codec 解码、单例限制及依赖顺序均在同一资格入口检查。Profile 只组装作者包，不再自行
拼装一份静态兼容性判断。ProjectScenes 以完整配置 TypeToken 识别计划提供者；其它依赖渲染
的 System 不会被当作 RenderSystem 配置解码。

RenderSystem 只负责 Render Scene 的创建/删除、行为构造、阶段遍历和帧封包边界。
它不包含 Mesh、Material、Light、Camera 或专业 Command。内置转换在此模块登记；
扩展作者可以使用 scene_render 的模板和 concept，不必链接此内置模块。

维护阶段先采用实际完成结果，再在稳定点准备帧数据，最后向现有 Main FrameInput 封包。
同步帧操作不创建 Sender；异步资源工作交给共享 ExecutionRuntime。没有新线程池、队列或 Scope。
每个行为的封包以现有 FrameInput 命令组结束，按声明依赖顺序接纳。不同角色可能写同一目的地，
不能把它们擅自视为并行无冲突；行为内部可以批量提交相互独立的命令。这不增加 GPU 完成等待。
`whenReady()` 在能力安装和 EnTT 连接完成后返回；调用者必须持有 Scene lease，直到采用或清偿。

## 声明与事件

组件关联声明 `SourceComponents`、`ResultComponents`，能力操作声明实际 Scene/View 范围。
默认由自身脏数据决定是否封包；需要持续发布的操作显式声明 `each_frame = true`。不再保留没有
独立执行语义的 CHANGES/STRUCTURE 枚举。
`Dependencies` 声明单一行为借用：优先同角色，否则要求唯一候选。需要多个角色时显式声明
`AggregateDependencies`，只聚合本次已选中的角色，构造时通过 `context.forEach<T>()` 取得借用。
未选中的备用实现不会被聚合顺带启用；帧内不重新做名称/类型发现。
冷态按依赖排序构造，停止时反向清偿，最终析构及部分构造回滚也显式逆序。Scene 构造不能借用尚不存在的 View 行为。
有状态行为提供明确的 fallible factory；无状态、空且可平凡析构的函数对象可以省略 factory，
不会为每个 Scene/View 分配一份空对象。需要保存任何依赖或异步状态时必须使用实际对象。
模板连接 EnTT `on_construct`、`on_update`、`on_destroy`，并折入连接前已有的组件。
连接以现有 RenderBindingInstance 中对象持有槽的地址为身份，即使无状态函数共用静态对象，
不同 View/角色的连接也不相互覆盖。持有槽预留后地址稳定，连接先于对象销毁。
信号直接调用类型化入口，热路径不按组件 Hash 派发；已删除无消费者的 affected/反向索引。

作者或系统写入必须使用 `registry.patch<T>()` / `replace<T>()`；直接修改 get 返回的字段
不会发出通知。观察者只记录需求、脏实体或退出句柄，不在信号栈里改组件、等待 IO 或提交资源事务。
Scene maintenance 仅在允许结构修改的安全点装入结果。结果组件不进入作者序列化或 Inspector。

## Mesh、材质与资产

`MeshRenderBinding` 处理 Mesh3D 的资产需求、候选替换及退出。`MeshRenderResourceComponent`
按冷态角色槽保存完整的 Mesh/Material/Instance 采用结果；替代原来三份平行组件。常见单角色
内联保存，只有多个角色才扩展数组。一个角色只修改/解除自己的记录，实体 on_destroy 时各绑定
从仍可读的组件中取出本角色句柄，不能释放别人的角色。候选集合只包含尚未完成的需求。
内置 Mesh 与 Transform 操作使用 `raster.mesh` 角色；相同专业类型可按其它角色再次登记。
各角色实例身份独立，实际相同 Pipeline/目的地/资产版本仍共享上传表示。
单写者资格在本次选中的 Plan 上检查，不在全局登记时排斥互斥能力的备用实现。
只有明确声明 `partitioned_results = true`、实际按角色/View 分区的实现才能同时写同一结果组件。
`Dependencies` 表示构造借用；观察结果组件只连接 EnTT，不隐式建立构造依赖。因此 Scene 观察者
可以在 View 创建之前连接，随后收到 View 产生的组件事件。

`resourceInputs()` 声明工厂实际借用的资源输入。每项包含 schema、SCENE/VIEW/ASSETS 来源，
以及可选 implementation/index；缺省选择必须唯一。选择值不保存未来 Scene/View 的代际 ID。
`inputs()` 仍表示能力匹配合同；仅通过已有生产者更新资源的操作不再重复声明目的地。
Transform/Light 更新操作从所依赖的 Mesh/Light 生产者借用准确目的地。

安装前从共享资产输入中只选出当前 Plan 使用的生产者，再调用 InstallCapabilities。安装后
`resolveRenderBindingInputs()` 根据本次 Scene/View 结果产生临时的工厂借用，不修改声明 Plan。
不能用 Scene 消费别名查询按生产者身份保存的资产表。多个同 schema 生产者必须明确选择，
Render 仍检查所选生产者与实际 Storage 的相容性，未选择的生产者不会干扰安装。

资产按 Pipeline、实际输入目的地、来源域和版本复用。CPU Cooked Material 与 GPU Material
使用不同表示表；后者引用已就绪的 Cooked 和 Texture，不重复读取或解码。
同 Pipeline/目的地再次安装时，Pipeline 的 `compatible()` 必须验证全部表示依赖（包括材质的
Texture 来源）。Scene/View 停止生产并清偿全部记录后，在结构安全点移除对应表；不能在完成
回调栈中删除表，也不把历史空表永久留到应用退出。Global 表继续由共享 RenderAssets 持有。

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
`ERenderOperationScope::VIEW` 声明在每个真实 View 采用时构造。构造上下文提供 ViewId、
来源实体和已构造的 Scene/同 View 依赖。`CameraInputOperation` 本身使用这条路径，每个 View
只有自己的变化标志，不再保存整组相机的脏集合。

View 提供者在结构安全点调用上下文的 `bind_view`，并在删除原生 View 前调用 `finish_view`
直到 COMPLETE。RenderSystem 只拥有这些行为对象，不接管 View/Target 的业务所有权。
局部退出将维护阶段的 stop token 换为一个共享的、已请求停止的常量 token；没有逐 View
stop source、Scope、任务目录或完成队列。已接纳异步工作仍由共享 ExecutionRuntime 清偿。
全 Scene 退出先结束 View 操作，再反向结束 Scene 操作；同 View 的 Resize/Effect 替换不重复构造。
构造函数只捕获依赖，不能在阶段开始前启动不可回滚工作，以便构造失败直接 RAII 回滚。

## 停止与共享引用

Scene stop 阻止新需求，继续清偿已接纳工作。依赖的反向顺序为：退出 Camera 输出，结束帧操作，
解除实例关系，再归还该 Scene 的资产引用；之后才删除 Render Scene。
连接先于行为对象析构，Registry、Requester 和共享 ExecutionRuntime 活得更久。

当前 LitScene 的材质目的地是 Global。停止 Play 只归还 Play 的引用，不能等待整张 Global
材质表清空；编辑 Scene 仍可使用它。RenderAssets 负责最后引用释放及异步逻辑释放，应用关闭
在全部 Scene 结束后清偿该设施。GPU 物理清理始终由 Render 唯一 cleanup_scope_ 完成。

模块代码必须覆盖行为、Pipeline 表及其最终回调。Scene Plan 唯一保存行为的代码 pin；所有
Scene/View 实例借用该不可变声明，实例和删除回调结束后 Plan 才销毁。
外部 Pipeline 可在 `RenderAssets::install(pipeline, destination, code_lifetime)` 中提供代码 pin。
它随实际表保存，晚于多态表析构及 foreign delete 返回释放；不按资源复制。内置静态代码传空 pin。
这提供卸载寿命保护，不提供运行中替换某个已安装 Pipeline 的产品接口。

## 资格与采用边界

创建预校验使用同一 Scene/Simulation 构造资格算法，包括版本、合同、配置解码和依赖顺序。
Render 的嵌套配置通过 ValidateCapabilityConfiguration 调用实际实现的 codec/依赖解析，
不创建 Storage 或 GPU 资源。真实工厂、IO、设备分配失败仍属于后续实际执行结果。

View 提供者先创建原生 View，再通过 `prepare_view` 安装其实际资源。有限候选拥有安装回复，
到结构采用安全点才调用 `bind_view` 构造行为。失效候选走原有退出路径；Resize 不重建行为。
局部 `finish_view` 的失败保留完整 SceneExecutionFailure，不能只提取可选的领域 cause。
原生 View 删除之前先清偿其行为和资产表；Scene 删除遵循相同顺序。
