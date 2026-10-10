# MA10：全局状态分类与模块边界审计

审计源码：`9de3a098cb71edcbdecdf21e49422477f35b2cb6`。仅生成分类，不修改生产实现。
规范 §15 的分类工作完成；这不是所有现存全局状态的并发安全证明，也不补足 MA08/MA11 的未测项。

## 审计方法

核对 1,291 个 tracked 第一方活动 C++ 源/头，排除冻结 legacy、测试及 vendored 实现。
保留全部 static/thread_local 词法候选，并区分静态成员函数与对象；跨行声明一并读取。
记录 86 个非 constexpr 状态/声明位置和 1,011 个 constexpr/consteval 候选。
补查已知无 static 关键字的 namespace 状态和 instance 入口；常量/函数候选数量不当作 singleton 数量。
每项附固定源码哈希、路径、行号、分类和理由。词法筛查不是 C++ AST 或自动正确性证明。
六处用户差异均使用审计提交的 tracked 原体，不纳入或覆盖用户工作区字节。

## 分类与保留依据

| 分类 | 实际状态 | owner 与边界 |
|---|---|---|
| PROCESS_SERVICE | ObjectRuntime | Object 动态库的唯一实例；一个进程一个 affinity domain。关闭前停止并 join producer，再由 owner 排空已接受消息。对象生命周期仍由 C++ owner 决定。 |
| PROCESS_SERVICE | ErrorRegistry | Error 动态库拥有不可变描述；线程安全登记/查询，不保存插件 formatter 或具体业务错误对象。 |
| PROCESS_SERVICE | ReflectionRegistry | Meta 动态库保存显式初始化的 registry 指针；宿主负责 init/destroy，已有 metadata/code owner 生命周期保留。不是新热路径的默认底座。 |
| PROCESS_SERVICE | KernelRegistry / FrameExtensionRegistry | 两者分别拥有 kernel 定义和唯一 extension slot 发号；MA05 后不再重复 extension map。Kernel 定义继续持有代码 pin。 |
| PROCESS_SERVICE | RenderErrorRegistry | 原 renderer 结构化错误描述机制，继续通过既有转换接入 Error，不重写该边界。 |
| PROCESS_SERVICE | log::State | 原子发布非拥有稳定 LogOutputTarget，host 保持 target 至 producer 停止；不是 callback 历史保留表。 |
| PROCESS_SERVICE | EditorWindow 的 GlfwRuntime、GLFW runtime_owner/display_revision | 产品唯一 backend owner；底层指针只检查排他性，monitor revision 是平台事实。保持现有 owner-thread 合同。 |
| PROCESS_SERVICE | Script/Scene/Render/Input 身份源、编译模块/temp 文件序号、DLL shadow-copy once/序号 | 只发身份/名字，不拥有 Session、Scene 或业务结果。没有因为这些计数器而制造单例业务服务。 |
| STATIC_CODE_METADATA | 内置节点/系统/插件/kernel 表、错误登记结果、Lua codec plans、默认值及回调 Ops | 静态定义或派生只读投影；定义与动态实例分开。动态定义依旧使用实际代码寿命合同。 |
| RUNTIME_DOMAIN | Object callback depth、Render producer lane/thread token | 原线程域的重入/来源判断，不是另一个 ObjectRuntime 或 RenderRuntime。 |
| RUNTIME_DOMAIN | Animation 和 Shadow TLS scratch | 容量复用，原算法每轮覆盖；不能将它们当成可重入或跨帧持久世界事实。 |
| HISTORICAL_CONVENIENCE | MetaModuleRegistrar pending 链 | 保留现有生成消费者。magic-static 初始化保护不等于链表修改线程安全；加载/排空仍按原宿主串行合同，新扩展走显式登记。 |
| HISTORICAL_CONVENIENCE | World/Simulation 描述 failure injection、Snapshot 测试计数、Render 生命周期诊断计数 | 记录真实测试接点及宏边界；不当作产品服务，也不为本次审计删掉原失败验证。 |

SceneRuntime / RenderRuntime 仍是 factory 创建的显式 owned instance；Input、ExecutionRuntime、TaskExecutor、
UploadLifecycle 仍由既有实例持有各自状态。不因“去 singleton”改写这些并发、退休或输入机制。

额外需保留的审计风险：Flow BuilderContext 的 32 位模块符号序号采用 fetch_add，当前没有耗尽分支；
ReflectionRegistry 的重复 init/未初始化 instance 仍依赖显式宿主合同。本次未取得违反这些前提的实际产品复现，
不宣称已修，也不在默认只读 MA10 中顺手变更身份或注册算法。

## 实际依赖图

从同一实现的 Editor 427 个、PLAYER 372 个 CMake File API 目标读取真实 source provider，
分别核对 32 个第一方 modules 库的直接/传递库边及编译 include 路径。
未发现反向依赖 engine/editor 的模块运行库边，也未发现活动 modules 对 EditorContext、LuxEngine、
ProjectManifest、PluginManager 的产品符号引用。

`render_features` 确有资源生成顺序依赖：utility → lux_shader_emitter/lux_asset_packer → toolchain 库。
这是已登记的 build-time 工具执行；实际 render_features 链接命令不含这些工具/库。
审计保留该路径，不通过忽略整个 target 或字符串替换把它消失。
Imported/third-party 及运行期动态装载不能仅靠 File API 推断，其 SDK/DLL 资格引用对应真实运行记录。

## 证据与范围

外部快照：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma10/evidence`。
包含分类脚本、完整候选/哈希、86 项状态来源和两种实际 File API。
1101 个文件，manifest SHA256：
`0d8e9290c16988cb1317bdfd3b7f0e168d2a87dfac4cccd9aa5647fb055a2dc5`。中文/空格路径搬迁、缺失和篡改拒绝通过。

没有新运行成绩由该只读审计产生。实际 Editor/PLAYER/SDK 成绩见来源适配验证记录，仍绑定其实现 SHA。
MA08 图编辑器呈现、MA11 冷构建/最终矩阵与 sanitizer 等必测范围不能被这份分类替代。
Linux 未提供环境、原生输入延期、历史 transfer_idle/minimize/WAR 等原状态不变。
唯一施工状态继续维护于 `.internal/editor-redesign/terminal-architecture/STATUS.md`。
