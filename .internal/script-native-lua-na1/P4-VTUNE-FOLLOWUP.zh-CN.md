# P4 补充 VTune：长任务退化的实际调用路径

本次按用户追问补采 P4，一对 A/B，使用原固定镜像，32 个 EXE/DLL 文件重新核验 SHA。生产 A=f7d2815b，B=4a8812e9；没有改生产源码、VM、编译参数或正式计时结论。先前六份有效采样只有 P1/P3/P5，不能把它们重标为 P4。

每侧实际 10,000 实例、16 warmup/64 measured waves、640,000 完整任务、20,480,000 次等待/resume；payload31、逐实例79361、errors0/backlog0/leases0。affinity16，ITT 在实际波区间启停。P4 A 为 640,000 个 thread、20,480,000 次 Lua resume；B 对应任务 thread/resume/release0。

## 结论与范围

长任务已将 A 的 thread 创建摊到 32 次等待；B 则在每次原生恢复后重新进入一次完整同步 Lua 步骤。当前桥带来重复形状验证、两次资格 capture、参数槽/registry读取、protected call、Lua函数调用及退出恢复。**保存 Lua thread 生命周期的收益按任务发生，而新增同步边界按每轮发生。**

这是当前 NA1 实现的成本，不是“C++ coroutine 必然比 Lua 慢”，也不是所有剩余开销已被证明为必要安全成本。已经确认有新的大块边界开销；哪些小项能删除、能回收多少净差额，尚未做因果消融。

## 同量软件采样

| 项目 | A | B |
|---|---:|---:|
| app 内 ROI wall 秒 | 7.5609561 | 8.5910481 |
| VTune ROI CPU 秒 | 6.695179 | 7.951984 |
| thread 创建 | 640,000 | 0 |
| 实际等待 / resume | 20,480,000 / 20,480,000 | 20,480,000 / 20,480,000 |

这是一对有采样扰动的定位运行，不替代此前三对无采样正式结果 +23.90%/+28.88%/+29.41%。正式每完整32等待任务 A/B 中位11.454/14.775μs，相当于每轮摊销约358/462ns；这不是纯 coroutine resume 单函数成本。

B 调用树包含时间：

| 节点 | B CPU 占比 |
|---|---:|
| Tasks::step（包括 typed 包装和整条同步调用） | 40.06% |
| ScriptCoroutineContext::invokeSyncStep | 37.06% |
| LuaScriptBackend::Impl::invokeSyncStep | 29.30% |
| executeSyncStep | 20.61% |
| 其内部 lua_callk（实际进入 Lua 函数） | 16.90% |

这些是嵌套节点，不能相加。Tasks::step 减去内部 lua_callk 子树，约 **23.16% / 1.84 CPU 秒**落在外围 typed 包装、验证、封送、进入/退出等路径。此差值还不是相对 A 的纯净新增量，因为 A 的 resume 也有输入/资格处理。

A 的 stack_init 包含约0.60%，traversestrongtable 约0.59%；这两项没有代表所有 allocator/GC，却清楚表明 P4 不再呈现 P1 那样的任务创建热点。B 的 resolveEvent 自耗0.317088秒/3.99%，每次等待仍按 instance slot→contract→import slot→admission 解析。核心 Event/Ready 工作未被新路线删除。

B 的 _intrinsic_setjmp 自耗约0.057378秒/0.72%。lua_pcallk 包含23.68%是整棵受保护执行子树，不是 setjmp/错误保护本身花23.68%。本次不支持把退化都归到 longjmp。

## 实际每轮调用

A：`核心 resumeOne → Lua resumeLuaContinuation → 向原 thread 推入 payload → lua_resume → 从原循环暂停位置执行 self.value+=payload → 下一次 Event wait/yield`。self 和函数执行状态留在原 thread；正常中间轮不重建同步函数进入上下文。

B：`核心 resumeOne → CppStatic resumeCoroutine → prepareResume/activate → 编译器 coroutine.resume → callStep/参数槽 → C++ invokeSyncStep → Lua invokeSyncStep → ExecutionScope/traceback/pcall → executeSyncStep → registry取函数和self/压payload → lua_call → 恢复base/资格复验 → C++下一次wait/resolveEvent`。

当前结构性重复的具体位置：

- [CppStaticScriptBridge.cpp](../../engine/domain/simulation/scripting/cpp_static/src/CppStaticScriptBridge.cpp) 第19行开始：view完整身份/publication/current、ordinal、参数/返回数量、type_id/kind/size/alignment/pass；获取 qualification；用户代码后复验。
- [LuaScriptBackend.cpp](../../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp) 第1876行开始：再次检查参数/返回数量和每槽 type_id/kind/size/alignment，再 capture qualification。第1832行内层 callback另做stack额度、registry函数/self取值、参数转换和原资格检查。
- 两端范围/类型检查确实重复进入当前热路径。它们不是 Lua 动态 table 字段读取本身；移走装配期不变结构检查是候选方向，不能据此删掉当前权限和用户代码后的失效检查。

Lua 内部 table 查找仍很显著：A luaH_Hgetshortstr 自耗0.996219秒，B1.144015秒。仅有软件样本，无法把此差异精确判定为 hash布局/cache/分支原因；不将它全部归给检查，也不声称去检查即可拿回全部28.88%。

## 后续取舍

若继续这一路线，优先压缩已准备的 typed step 边界：装配期绑定签名/slot布局，明确内部已验证入口；在一个有效调用借用内复用原资格，避免两端分别完整capture；保留用户代码后的原身份/权限复验、Lua错误保护和内层stack额度。之后才评估Event映射和细小叶函数。这里没有实施优化，也没有承诺必能追回全部回退。

对多次等待但每步只做简单Lua计算的任务，继续保留原 Lua coroutine 有当前证据支持。对 P5，另外还有 record 在 before/after 两次封送的问题；它不能解释本次 scalar P4。

[原始补采 ZIP](evidence/p4-vtune-followup.zip)（含 DB、调用树、命令、stdout、身份和 analysis.json），[逐文件哈希与身份](evidence/p4-vtune-followup.json)。ZIP SHA-256：`a6093cd9ff8f132035d611874cfadfec5e464da2f2e050485478e1937d02e530`。原 NA1 总归档不改，补采不伪装成原资格。
