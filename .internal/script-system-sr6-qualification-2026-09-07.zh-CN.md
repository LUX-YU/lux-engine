# SR-6 联合候选与资格

入口为72b46285，生产参照E=f64cadde；依赖lux-cxx 3100f54d、toolset 99c3d048保持。
本轮明确批准SR-6，保留已量化成本，不宣称性能等价；不扩类型、合并main或打发布tag。
实际工作位于既有s5/source分支，新资格目录为build/RelWithDebInfo/s6；main七项未知文件与E产物已固定哈希。

## 开工职责映射

| 协调/唯一owner | 实际状态与正式入口 | 失效与清理 | 对应真实验证 |
|---|---|---|---|
| ScriptSystem | prepare、processLifecycle、executeStablePoint、shutdown；跨owner顺序 | 先撤权限，等待保护；统一故障记录 | lifecycle、hook execution、move assignment |
| ScriptInstances | mounts_、invocation_states_、methods_、changes_；装配票据、调用票据、query/collect | 完整代次及原窗口；领取一次回收资格，资源结束维护reclaimed | lifecycle/reclaimed、admission、七点重入 |
| ScriptBindings | endpoint目录、配置/绑定范围、handlers、token、pending_unlinks_ | 发布回滚、逻辑失效与延迟unlink；busy保留token | bindings rollback、跨批次顺序 |
| ScriptPreparer | 资产resolver、prepared capability目录及准备票据 | 提交前回滚，提交后归Instances；目录覆盖实例寿命 | prepare/BeginPlay失败与资源计数 |
| ScriptExecution | execution_instances_、active_hooks_、Continuation/Awaitable、唯一最终结果及ResumeRing | 来源双向取消、copy pin、完整身份、取消/退休/销毁 | continuation、event、pin、frontier、预算 |
| ScriptEventWaits | waiter来源、实例索引、路由/登记序号、claimed_ | occurrence claim→callbacks→complete；取消和unlink | sealed batch、嵌套claim、跨reset结果 |
| ScriptTimers | NextStep来源、模拟时间heap_及instances_索引 | 显式来源取消及物理槽位复用；不改deadline/稳定点 | Timer取消、同deadline顺序、容量 |
| ScriptCompletionIngress | 外部transport、准入lease、队列/frontier和关闭 | 不拥有最终结果；关闭/迟到/旧代次拒绝 | ingress并发、frontier、背压 |
| Lua值模块/backend | 生成codec、typed slots、protected primitives、prepared projection | 初始资格；可重入转换后原资格复验；逆序清理 | 真实生成/runtime；本轮安装provider纵向链 |

本表按本轮源码复核，后续补入C01—C13最终断言、安装链与实际结果。既有SR-1旧阶段缺口不直接当作现存失败。
