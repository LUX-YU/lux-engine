# Editor 设施借用

`EditorFacilities` 只保存已经存在的领域对象的 `TypeToken → pointer` 对照。
它不拥有对象，不执行工厂，不解析依赖，不接入 PluginManager，也不在查询时分配。

Editor 的装配入口是 `EditorComposition::bindExtension()`，内部使用
`EditorFacilities::Composition::bind()` 建立候选表。Engine 不暴露这个查询表；其应用与帧入口
通过 `EngineAssembly` 显式借用，Scene 通过自身的类型化构造参数获得依赖。
Context 创建时一次发布；运行中的表没有 bind、erase、替换或赋值接口。
重复类型拒绝，不覆盖原地址；同 hash 不同类型名称也拒绝。

被借用的领域对象必须先构造、后于 Context 析构，且中途不得移动。
动态模块的代码和描述存储仍由领域原有 owner 保活；表不是代码租约。
Context 的 const 查询只返回 const 扩展面。未绑定类型返回 nullptr，不触发惰性构造。

`EditorServices` 仍负责项目服务工厂、惰性实例和逆构造顺序释放。
扩展表不查找服务，也不把某个服务自动登记成扩展面。
各领域的 Registration、Catalog、验证与具体操作继续留在各自模块。

有真实异步退出的项目服务通过 `closeAsync()` 声明完成，使用既有共享 ExecutionRuntime。
EditorServices 逆构造顺序等待这些入口，保留已完成事实；普通 RAII 服务不需要空关闭方法。
服务拥有者覆盖关闭回调及其尾栈；项目 Context 结束后才释放插件代码。
