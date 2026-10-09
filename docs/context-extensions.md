# Context 扩展面

`ContextExtensions` 只保存已经存在的领域对象的 `TypeToken → pointer` 对照。
它不拥有对象，不执行工厂，不解析依赖，不接入 PluginManager，也不在查询时分配。

装配代码使用 `ContextExtensions::Composition::bind()` 建立候选表，再将该构造值交给
`EngineContext::create()`。Editor 的装配入口是 `EditorComposition::bindExtension()`。
Context 创建时一次发布；运行中的表没有 bind、erase、替换或赋值接口。
重复类型拒绝，不覆盖原地址；同 hash 不同类型名称也拒绝。

被借用的领域对象必须先构造、后于 Context 析构，且中途不得移动。
动态模块的代码和描述存储仍由领域原有 owner 保活；表不是代码租约。
Context 的 const 查询只返回 const 扩展面。未绑定类型返回 nullptr，不触发惰性构造。

`EditorServices` 仍负责项目服务工厂、惰性实例和逆构造顺序释放。
扩展表不查找服务，也不把某个服务自动登记成扩展面。
各领域的 Registration、Catalog、验证与具体操作继续留在各自模块。
