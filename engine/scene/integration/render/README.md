# Scene 的 Render 数据接线

此模块实现 Engine ECS 数据到 Render 公共能力 Payload 的转换。Render 不读取 Registry；
专业行为也不访问 Render 内部 Feature 或 Storage。应用使用 `registerBuiltinRenderBindings()`
登记内置声明，将稳定的 `RenderBindingDeclarations` 作为 RenderSystem 构造依赖传入。

## 声明与事件

组件关联声明 `SourceComponents` 和 `ResultComponents`，能力操作另声明更新策略和执行范围。
通用模板连接 EnTT `on_construct`、`on_update`、`on_destroy`，连接后折入已有源组件。
运行期信号直接调用类型化入口，不根据组件 Hash 再派发一次。

作者或系统写入必须使用 `registry.patch<T>()` / `replace<T>()`；直接修改 get 返回的字段
不会发出更新通知。观察者只记录需求、脏实体或退出句柄，不在信号栈里修改组件、等待 IO 或提交资源事务。

## DirectLight 数据链

`DirectLightBinding` 处理 Light3D 与 WorldTransform3D 的存在性、资源创建、替换和退出。
异步工作由共享 ExecutionRuntime 的 stdexec Scope 持有；返回的数据先保存在有限事务中，
仅在 Scene maintenance 的结构安全点装入 `LightRenderResourceComponent`。
隐藏组件保存已采用 LightId、类型和实际冷态阴影输入，不进入作者序列化或 Inspector。
同实体移除再添加由需求代次区分，过期的成功返回只能进入释放路径。

`DirectLightOperation` 观察作者值、世界变换和已采用关联，仅处理脏实体。它直接将当前值写入
本次 FrameInput，不保存第二份完整光源值表。位置、强度和颜色更新不重建资源或编译图；
阴影描述变化才改变冷态输入。帧失败后沿 Requester 原有 resynchronization 重新提取。

替换时旧关联保留到候选实际成功，失败不假装回滚已发生的 GPU 事务。旧 LightId 的逻辑释放
还必须等当前和正在退出的 View 不再使用旧冷态输入；被确认的 revision 不得覆盖较新改动。
这一条件只控制未来引用，GPU 物理寿命仍由 Render 唯一 cleanup_scope_ 处理。

## 停止

Scene stop 阻止新事务，继续消费已经接纳的完成。所有关联行为静止后，RenderSystem 才移除
Render Scene；Scene 的资源作用域负责清除剩余逻辑资源。EnTT 连接先于行为对象析构，Registry、
Requester 和共享 ExecutionRuntime 必须活得更久。没有新增线程池、完成队列或局部清理 Scope。
