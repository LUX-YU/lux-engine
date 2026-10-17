# Editor Framework v2

Editor 按 app、window、context、project 和 ui 组织，产品入口与项目操作见
[Editor 使用说明](../editor/README.md)。底层依赖方向为 Editor → Engine → modules；
`editor_legacy` 只供历史参考，不进入当前构建、链接和安装。

## 对象与 UI 所有权

Object DLL 提供进程共用的 ObjectRuntime，宿主在启动工作线程前建立其对象线程。
ObjectId 是非拥有的生成式身份。LuxObject 的 parent 只表示结构，不决定 C++ 对象删除。
跨线程交付使用现有 Object 通路，接收者失效与完成回收仍由该 Runtime 处理。

ui::Root 唯一拥有顶层 Pane；Pane 只有一个内容根。Root 枚举时借用 Pane，不转移所有权。
Element 使用 ObjectId 表示运行期 UI 身份。Root 内部只有一份 DockTree 模型；布局变更
先验证、准备，再提交，失败不发布部分结构。

PaneHandle 由 Root 身份和局部生成式 PaneId 组成，不延长任何对象的寿命。
移除、重新挂载、替换 Root 或地址复用后，旧 Handle 失效。resolvePane 只允许同步 Owner 线程借用。
运行期 Handle 不是持久布局格式。

PaneChanged 和 ObjectRemoved 是不可复制的同步借用，不能通过排队延长有效期。
UI 的结构变更经过自身 API；不能绕过这些规则直接使用通用基类变更结构。
Root::update 采用排队结构变化；绘制和捕获通过明确的帧入口，维护更新不隐式绘制。

## 项目装配与退出

宿主拥有 Window、EngineContext 和产品 Assembly，项目切换不重建这些基础设施。
EditorComposition 构造声明，完整 EditorContext 发布后提供不可变目录和惰性唯一服务。
Pane、项目服务和 Context 的实际关系及异步项目采用规则以
[Editor 所有权合同](../editor/README.md#ownership-and-lifetime) 为准。

TaskScope 析构只请求取消。已接纳工作保有自己的输入、结果与任务组，不能借用会先销毁的 Context、
Pane 或可变目录。项目关闭不等于线程 join；宿主最终退出必须停止准入、排空完成并 join 生产者。
Object/UI 的同步回调以 noexcept 作为合同；普通堆 OOM 为 fatal。

## 错误与扩展

错误 DLL 拥有不可变描述。错误值只包含稳定 ID 和数值参数，不借用插件载荷。
描述在装配期登记，失败点不注册字符串、不获取登记锁，也不为诊断创建新的资源 Owner。
本地类型错误和领域诊断继续留在各自模块；第三方异常只在明确边界转换。

Context 扩展只借用已有领域对象，见 [Context 扩展面](context-extensions.md)。
Pane 之间通过实际项目/领域服务协作，不保存互相的业务指针，不增加全局 Element 目录或事件总线。
SceneToolRegistrar 的选择语义须由实际工具消费者确定，不能把预留接口当作已完成的编辑工具系统。

公共头和独立安装消费者分别位于模块 include/ 与 editor/tests/installed/。
库不借助 RTTI；模块内 pinclude/ 和项目级 sinclude/ 不是安装 API。
[持续质量规则](editor-quality.md) 规定寿命、错误、发布、依赖和验证边界。
