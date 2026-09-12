# P4 热路径逐项审计：静态事实、数据搬运与动态工作

本轮只读审计，未修改生产代码、VM、测试或编译参数，也没有新性能候选。依据为生产4a8812e9、P4实际软件采样，以及同一资格产物的优化COFF/机器码。原正式回退数据不改。

## 结论

确有相当一部分工作应在冷期完成，当前问题也超过参数校验：**静态typed参数被转成通用ABI描述，每次重新计算/写入，再由两个后端边界解码、自检。** 同步桥还多了一层C callback、成功结果包装和多次资格获取。

[逐项清单](P4-OPERATION-AUDIT.csv)共32个工作组：5项可直接整理，7项需补冷期绑定/私有入口，4项可复用已定位或已验证关系；5项应作为受限实验，2项尚无充分机器码证明，9项必须保留或本来就没有所猜测的开销。这是工作组数量，不是32条指令，也不是可相加的独立性能收益。

优先消除元数据往返与同步边界结构，再考虑小叶函数。不能因为32等待回退28.88%，就声称清掉以下清单一定收回28.88%。

## 一个关键缺口：冷期还没验证真正的C++调用签名

[NativeLuaTaskBackend::construct](../../engine/domain/simulation/scripting/native_lua_tasks/src/NativeLuaTaskBackend.cpp)验证了plan中的步骤签名与Lua制品导出相同，也验证了任务导出和import子集。但是[CppStaticContract](../../engine/domain/simulation/scripting/cpp_static/include/lux/engine/simulation/scripting/cpp_static/CppStaticScriptContract.hpp)只有exports/Ability/Event等，没有同步步骤的typed调用点声明。

所以`callStep<void(int)>(ordinal, value)`的实际模板签名与ordinal组合，目前首次在调用时才被比较。当前“错Signature/错ordinal”真实负例依赖这次拒绝。要把第一层检查搬出去，必须先使实际C++步骤需求成为冷期输入，或者让冷期工厂返回不可伪造、绑定完整实例/发布代次的typed handle。任意runtime ordinal接口仍应经过checked边界。仅凭Lua plan检查过，就删除C++调用点检查，会放掉真实坏输入。

完成这个绑定后，私有热入口可以直接使用已准备的参数推送方案和实际值；无须每次以type_id/kind/size/pass描述自己。保持七组件所有权、单调度器和原错误处理，不引入public trusted开关。

## 机器码确认的静态工作

1. `syncStepSlot<int>` 每次执行7字节FNV循环，使用prime `0x100000001b3`，有真实load/xor/imul/jne。`typeId(Traits::CanonicalName)`写在模板里并不保证常量折叠；此处没有折叠。应强制`constexpr`求值。`await_suspend<int>`设置expected_type时也有同样的循环；期望类型仍随每次实际等待更新，但不用重新算哈希。
2. C++参数匹配和Lua参数匹配都执行动态alignment取模。COFF中确有`divq`。P4 int32→void正常路径各执行参数循环一次，合计2次除法；返回循环的另一个除法在void路径不执行。typed native对象的对齐/大小可在绑定时证明，公共erased输入仍需验证。
3. 24B的`lux_script_value_slot`、40B的`lux_script_call_frame`、16B的Passes span被构造、清零和复制。它们大部分字段是固定类型/数量/空返回，只实际payload值/地址在变化。可改为冷期描述+本次值地址，不能用全局可写scratch去破坏重入。
4. `Tasks::step`把成功expected转成`ScriptSyncStepError{ordinal, BACKEND_FAILURE, 0}`，再复制12B并检查status。实际机器码存在这些写入。成功应只流转最小状态，错误才组装详细结构并进入原fail协议。
5. P4返回void，通用Lua request仍为80B并带output及返回分支；scope类别、参数推送种类、返回形状可绑定到窄适配器，减少通用分支/未用存储。

由已核对成功路径乘以本组20,480,000轮，推导出40,960,000次静态类型哈希和40,960,000次参数对齐除法。哈希共有286,720,000个字节轮次。**这些是代码路径与真实业务量的静态推导，不是新分支计数器或PMU计数，不据此捏造耗时。**

## 资格与生命周期：可以复用，不可以缓存ACTIVE跨用户代码

P4正常绑定路径有2次capture、5次显式qualification.valid、2次composition current。它们并非5种不同权限需求。

- C++入口到Lua入口之间，已完成结构解析且没有用户回调的段落，可传递同一原始资格，免去第二次capture和紧邻的重复检查。
- Lua返回到C++返回之间没有新的用户代码时，可合并相邻后置检查；仍必须同时验证原发布代次与原权限，不吞掉脚本已返回的非零错误。
- 栈扩容、allocator回调、自定义converter、Lua函数本身都是需要考虑的失效边界。**纯scalar不等于整个准备过程绝不可能回调。** 不能机械把5次valid改成“入口/出口各一次”，更不能在回调后重新capture一个新的ACTIVE。
- active_execution、准确base、嵌套深度与恢复原上下文仍要保持，不能把ExecutionScope扩展成覆盖任意原生任务代码的全局VM当前实例。

## 校验之外最大的结构候选：去掉中间C callback

当前scalar→void成功路径直接调用Lua API 14次（按源码和优化产物核对，非新增运行计数）：

| 外层7次 | 内层7次 |
|---|---|
| checkstack、gettop、rawgeti(traceback)、pushcfunction(callback)、pushlightuserdata(request)、pcall、settop(base) | touserdata(request)、checkstack、rawgeti(function)、rawgeti(self)、pushnumber、call、settop(1) |

更窄的内建scalar→void候选可尝试：`checkstack → 保存base → 放traceback/函数/self/实际scalar → 原资格复验 → pcall真正Lua函数 → 后置复验/恢复base`。这样约8次API调用，保留一个pcall，删除的是中间C callback的进入/返回、request传递和一层CallInfo，不是错误保护。

已核对固定Lua55源码：`lua_checkstack`增长失败返回0；`lua_pushnumber/boolean/lightuserdata`及`lua_pushcclosure(n=0)`只设置值；对合法registry的rawget不做metamethod/分配。但仍需在实现候选时验证栈增长的回调、hook/debug可见C栈变化、错误、OOM、TBC、嵌套和撤权。只能在允许的纯内建子路径采用，custom/record/可能分配的转换继续使用必要保护。当前没有实施或测量此候选。

较小的独立整理：私有固定traceback可直接`pushcfunction`免去一次registry查询；P4 void的`lua_call(...,0)`后原C callback栈已只余request，内部`settop(1)`可消除，外层base恢复保留。注意`lua_pushcfunction`当前本就不分配closure，不能把减少API调用误报为减少heap对象。

预绑定Lua函数/self到有界closure/upvalue也可以实验，减少registry lookup，但会增加每步骤根/对象，而且可能与直接pcall方案互斥。不能把两个方案的收益加在一起，不能缓存未root的VM内部指针，也不能透明复用返回table。

## 恢复和下一次等待还有哪些工作

- `prepareResume`已经验证当前packet的READY、type和size，`result<int>`仍再次走nullable packet/value链、零值fallback和通用inline/spill分支。P4机器码可见。私有typed成功恢复读取可复用前面的证明和已定位数据；入口动态packet检查保持。
- C++ Event每轮仍从instance slot经过contract、association局部索引，再取实际admission；P4 `resolveEvent`自耗3.99%。可在冷期形成按import排列的实例关联。不能移除原core scope/instance/layout epoch验证，也不能让另一contract的source借用权限。
- prepared Event payload进入通用reserveAwaitable又检查固定布局，可用已准备来源入口与通用外部描述入口分开、共用动态接纳内核。source preflight→A→source commit和满容量错误优先级必须不变。本分支尚未合入H1，不能写成已经完成。
- 插入结果拿到key后立即find回同记录，可在owner内部返回临时定位结果。范围仅限没有用户代码且地址有效的段落；moving SlotMap跨用户代码指针禁令保持。
- ScriptStepContext内固定函数指针每resume重建、depth高水位更新、结果move和错误诊断symbol读取也列入审计。前两者需验证布局/统计合同与成本，后两者还不能据源码断言编译器没消除。共享core优化若双方都适用，应双方重测，不记成NA1独有收益。

等待槽的占用和释放、结果从A移出到有效owner、pin、取消、来源队列、claim cutoff、Ready FIFO/stale预算都需要保留。不能借优化恢复数据访问回到A1 cell模型或提前复用物理槽。

## C++内联和frame放置的修正

VTune里的模板名是逻辑inline栈，不等于相同数量的真实call。完整P4 ResumeCoro机器码显示step模板已经部分展开，仍有真实`syncStepSlot`调用、DLL invoke入口和Lua API调用。

同时，MSVC把部分ABI暂存放在coroutine frame内（可见以coroutine基址访问`+0xf8/+0x110`等），P4实际frame352B。源码把同步工作抽进helper，不保证优化后物理上全放普通栈。之前NOTES关于“全部属于普通调用栈”的表述过强，现已修正。正确方向是减少临时表示和活跃范围、再看最终机器码；受限out-of-line helper是否值得，要同时比较多一次call与较小frame，不能机械增加forceinline，也不关闭GS/CFG。

## 保留限制

上述16项有具体整理/绑定/复用方案，不代表16项都能独立直接删除。5个候选需要实际错误/重入与成本验证，2项没有充分最终机器码证据。现有Lua表读写仍是业务；luaH_Hgetshortstr差异没有PMU因果解释。实际Lua返回值和Lua调用Ability收到的动态用户输入，不能按C++typed静态参数的理由免检。

本轮不修改运行时，不重新跑矩阵，不声称已经回收任何纳秒。[审计CSV](P4-OPERATION-AUDIT.csv)、[机器码/VM源码摘录归档](evidence/p4-operation-audit.zip)、[身份与逐文件哈希](evidence/p4-operation-audit.json)可直接复核；P4实测见[补采报告](P4-VTUNE-FOLLOWUP.zh-CN.md)。
