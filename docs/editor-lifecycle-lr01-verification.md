# Lifecycle LR01：完整构造资格

LR01 Windows 阶段门禁 **PASS**。固定实现为
`427b2bb13e4641ac1dcc09be78c651de7e15ebd0`，lux-cxx 为
`0a0e7419fc7229df6e372cd35a540249f92250ef`。本记录不代表 LR02–LR08 已完成。

## 实现与责任

| 实现提交 | 闭包 | 删除或替换的原合同 |
|---|---|---|
| `98aa663a8bc34188443e9def527111a9e298afd9` | Root | 先创建 Context/Impl，再发布 Object 身份；删除 `initialize()` 与构造空状态分支 |
| `3463a030b57c6cd8175653f7bce40f25b1fcc85a` | GLFW/Window | 完整 runtime/native 候选；删除 `valid()`、`init()`、`isInitialized()`、`initError()` |
| `fd0329a99e481f716019d3524fbbf610d7f85dbb` | Tray | 每窗口一个完整 owner；删除静态初始化与 GWLP_USERDATA/WNDPROC 接管 |
| `66e314f00bed5c701cfd19350fc03640f14e425c` | Physics2D | 配置校验先于世界创建；删除独立 `prepare()` 与 `prepared` 构造标记 |
| `427b2bb13e4641ac1dcc09be78c651de7e15ebd0` | CppStatic/Lua | 池、VM、注册与根表准备完成后构造运行态；删除 `valid`、`valid_`、`vm_configured` |

Root、窗口、托盘、Box2D 世界、C++ 存储和 Lua VM 均由实际 C++ owner 释放。
窗口的 native 候选包含回调清理；导出 deleter 支持跨 DLL 的派生工厂失败回滚。
托盘回调仅在同步 native 调用期间固定其物理 Impl，public owner 结束时立即撤销登记；
没有语义僵尸对象或新回收队列。Lua VM 先于 allocator 与错误回调捕获销毁。
Simulation 接纳完整 `unique_ptr`，失败保留候选；没有复制安装算法。

保留脚本 continuation、实例、执行作用域与 pending operation 的真实运行状态，
保留模拟时间、原点变换、查询、帧驱动、对象消息与 GPU 退休协议。
可移动 owner 的空状态仍表达 moved-from；没有把所有 `bool` 当作构造缺陷删除。
普通堆 OOM 仍为 fatal；Box2D 原生世界表耗尽继续返回真实容量错误。

## 修前证据及审计勘误

四项实际失败输出均保留：Root 失败创建额外消耗 ObjectId generation；原 SDK Tray
覆盖窗口 userdata；原 SDK Physics2D 拒绝非法配置却占用一个世界槽位（128 降为 127）；
原 SDK ScriptEngine 对非法 GC 配置返回带空 VM 的活对象。

最后一项是 LR00 词法分类遗漏：底层 `ScriptEngineImpl` 的早退构造被误归为局部判断。
本轮追踪 LuaBackend 的实际依赖后补入 C1 范围，并迁移唯一生产调用方；
没有在上层再加一个有效性标记。原 LR00 快照、数量和结论未改写。

CppStatic 新测试在修前的正确配置下已通过，属于行为保留与构造结构收敛，
不被登记为原实现失败。首轮 async begin_play、模拟时间、native 注入头顺序、
测试注册和重复 LuaBoundary 等夹具错误，及实际 nested deleter 导出错误均保存原输出。
错误分类见归档 `scripts/failure-classification.json`。

## 固定 SHA 验证

独立检出通过 `ValidateTrackedSnapshot`，源码干净；使用已有独立构建树增量刷新，
**不宣称冷构建**。SDK 安装到从不存在的全新前缀，运行时不回退开发 DLL。

| 验证 | 实际结果 |
|---|---|
| Editor 全量 all / 第二轮 | PASS / no work |
| Editor CTest | 76/76，包括实际 GPU、宿主关闭及托盘生命周期 |
| PLAYER 全量 all / 第二轮 / CTest | PASS / no work / 45/45 |
| 原安装框架消费者 | 16/16，含跨 DLL、公共宿主和真实呈现 |
| 独立 Window / Physics2D / Script SDK 消费者 | 1/1、1/1、2/2 |
| 真实窗口显式模式 | `--desktop` 创建、失败回滚、回调寿命和 placement 通过 |
| 12 个修改后公共头 | 新 SDK C++20 MSVC 独立编译通过 |
| 实际 source/include/link/install 检查 | Editor 608、PLAYER 552 个 TU；无 legacy/旧 SDK/源码私有头补全 |

新构造测试验证原生失败、部分准备清理、移动替换、容量恢复、Script VM finalizer 顺序，
而非仅检查工厂返回错误。原 `window-native --native` 命令实际只执行默认 CPU 路径，
不计作 native 资格；后续 `window-desktop --desktop` 有真实 native 输出。
独立头首轮漏写 Eigen3 的已声明 include 路径，失败保留；按真实安装 target 路径修正后重验。

相关公共头同步 Debug、RelWithDebInfo 和 Android include 前缀并核对哈希。
Android 仅同步，没有构建成绩。未新跑 Clang 全量或 sanitizer；原
`Q-P06-CLANG-GET_DELETER`、UBSan `NOT_QUALIFIED` 保留，LR08 总资格仍须处理。
源码/SDK 原生输入接管为 `NOT_RUN_USER_DEFERRED`，Linux、系统 IME、旧性能范围不改判。

## 归档与后续

外部证据：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr01/verified-evidence/`。
1,099 文件、108 命令记录；manifest SHA256：
`9b0b38da01c6ce9fe2f9d9e3c23f62775086320cf332c171ddb790e9e5749ce4`。
验证仅使用归档相对路径；中文/空格搬迁通过，删除及篡改真实 Script SDK 结果均拒绝。
修前原输出、开发 dirty diff 哈希、最终 clean SHA 明确区分。

工作区仍为 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。
Context/Pane 用户排版差异未提交；ProjectBuilder 补丁独立保存且未应用。
main 与历史快照不变。按连续实施授权进入 LR02，先补 Vulkan/VMA 叶子所有权，
不提前重做 aggregate、Process、Runtime 或业务框架。
