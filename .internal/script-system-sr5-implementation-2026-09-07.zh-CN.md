# SR-5 首批值转换实施记录

本轮依据用户批准的《SR-4 补正验收与 SR-5 首批值转换实施任务单》，入口 `764fc5d3`。
固定 lux-cxx `3100f54d`、toolset `99c3d048`；main `f0e8c3fd` 及五项 tracked、两项 untracked 修改原样保留。
隔离源码位于 build/RelWithDebInfo/s5/source；原移动赋值补正不重复修改，原证据包不重新标记。
本文随最终候选更新；最终资格和成本结论以本轮报告/原始证据为准，不能用入口 108/120/14 数字代替。

## 1. 开工补齐的三项接口

### 1.1 原调用资格与 provider 提交点

`ScriptBehavior::captureInvocation()` 返回只读借用 `ScriptInvocationValidity`，不暴露 Registry、State、Mount 或可写 ACTIVE。
内部 `ScriptRuntimeAccess::bindInvocation/invocation` 只由拥有实例的 Instances 装配。
快照包含受保护 Mount 的只读 context、完整 ScriptInstanceId、retirement epoch、调用类别及唯一 owner 的检查函数。
快照只可活在原 backend 调用的保护区内，不能保存到下一调用或跨挂起。

Instances 的普通检查读取当前 ACTIVE、完整实例、epoch 和关闭准入；不是 isAttached，也不是 sameIncarnation。
`ScriptSystem::shutdown` 在既有 stop 边界调用 Instances::stopInvocations，防止关闭因保护而返回 busy 后继续进入新 provider。
这只关闭新调用权限，不提前释放实例。后续原 shutdown 的实际 EndPlay 仍允许其独立生命周期资格。

BeginPlay 由 Instances 在 INITIALIZED 的受控 invokeLifecycle 窗口授权；EndPlay 由原清理领取资格在 RETIRING 窗口授权。
二者都核对窗口、身份、epoch，不能把“不是 ACTIVE”作为拒绝所有生命周期 Ability 的理由。
逻辑退休后的旧普通快照不会因为允许 EndPlay 而变成生命周期快照。
本轮增加 C++ ScriptBehavior 内部只读连接与 Lua prepared access 字段，相关宿主内部布局改变；generic backend descriptor 和 C ABI 操作表/签名不变。

后续权限补正见 [SR-5 权限补正记录](script-system-sr5-admission-correction-2026-09-07.zh-CN.md)；原 C0 证据仍绑定 6086e4a4。
当前 Lua adapter 每次在 current 中取得 prepared context/dispatch/local slot、原 ExecutionFrame 和本次有效资格，
包括默认 scalar、零参数和原 async scalar。hasInvocationAuthority 区分 standalone 与已绑定但失效的 core，不能用无效 capture 放行。
可能重入的转换结束后，`LuaAbilityProjectionAccess::revalidate` 再检查原资格和原 frame/prepared 关联，才进入 provider。
不能从嵌套帧重取另一 provider。原 scalar 叶读取不执行用户规则或 GC，不伪称它曾存在已复现漏洞。
用户提交尚未执行的结构命令不改变 Instances 权威；真正 Registry 销毁或关闭准入才使普通快照失效。

### 1.2 typed 存储与构造

`LuaValueSlots<T...>` 使用 `optional<T>` 的未构造存储；并非先构造 tuple<T...>。
只有取得完整字段/参数值后 emplace，成功后槽位才拥有对象；转移源对象按其正常移动语义结束。
析构显式按反向索引 reset，不能依赖标准库 tuple 的元素析构排列。
记录按 C++ 声明字段顺序读取与聚合构造，Lua 键遍历只验证形状，不决定构造顺序。
字段改名不改变成员指针及声明顺序。

默认记录读取构造全新 T，不写回任何现存对象；const 成员可聚合初始化，push 可读取它。
省略成员的自动 reader 不猜参数位置/默认表达式，要求明确 override/factory；不存在默认赋值所有类型的强制路径。
无默认构造、非聚合或有不变量的类型可在显式 read 规则中用 no-throw 工厂产生完整值。
工厂局部拥有对象在下一次风险 Lua 操作之前已移交/清理，不依赖 Lua 展开 C++ 栈。

值 codec 的构造资格与 Ability ABI 分开：现有通用 Ability 输出槽采用赋值，含 const 成员且不可赋值的 T
不能作为该返回槽的结果。本轮不改该 ABI；用可赋值 Pose 返回和 const record 参数分别验证。
普通返回仍满足既有 trivially-copyable 等条件；非平凡跨挂起结果协议没有新增。

### 1.3 单一表示与方向

`LuaValueCodec<T, Policy>` 先选择完整表示的 Rule：显式 LuaValueOverride → scalar → LuaGeneratedValue。
在选中的同一 Rule 内判断 read/push；push-only override 不会从 generated table reader 补缺方向。
普通 Ability 的要求方向在冷期通过 generated LuaValueOperation 与实际语义签名逐项核对，不按“是 struct”推定支持。

表示指纹使用明确 Policy 名称/版本、Rule 名称/版本、方向集合、已声明 TypeTraits 的语义名、布局和生成字段/依赖事实；
不用地址、typeid hash 或临时编译器名字。同布局、同字段而语义名不同的两类型有不同指纹，另有编译期负例。
用户改变表示须更新声明版本，实现修改则经源文件依赖重建。它是声明契约比较，不证明任意函数行为等价或解决 ODR 违规。
backend 冷期检查所有实际 contribution 与 erased push 目录的一致性；同一 semantic ID 的名称、布局、方向/表示冲突被拒绝。
一个 backend 采用一种 policy，不建立全局可写规则注册器。

## 2. Lua 保护边界与有限 frame

通用代码位于 modules/function/script/lua 的 LuaValue.hpp/.cpp，runtime prepared 关联位于 simulation_script_lua。
Reader/Writer 不提供裸 VM、Registry 或 State getter；类型规则只能使用有限值操作。
typed 递归、构造与析构在 Lua pcall 的外侧 C++ frame；内层 C trampoline 只有非拥有指针、索引、字符串借用和计数。
trampoline 不标 noexcept，防止实际 VM 选择 C++ 异常展开时越过错误的 noexcept 边界；Lua 错误始终由其内部 pcall 捕获。

```
原 core invoke/resume/lifecycle 保护
  → Lua C Ability entry 捕获原关联/权限
  → 外侧 typed 槽位
      → 已登记 C trampoline + 输入值根 → pcall → raw table/键/分配
      ← 状态/输出或错误；仅清理由该 API 创建的 scratch
      → 纯 C++ 构造/移交/逆序回滚
  → 原权限与关联重验 → provider（此处之后业务不能回滚）
  → 受保护结果 push → typed 槽位全部析构
  → 受保护错误文本或低成本 fallback → 原 Lua wrapper error/yield
```

LuaJIT trampoline 初始化使用 lua_cpcall，覆盖初次闭包与 registry 写入分配；Lua54 使用零 upvalue light C function 加 pcall。
热操作只从 registry rawget 已创建 trampoline，准备参数前用 fallible lua_checkstack。
数值/布尔读取不分配、不调用 metamethod；scalar push 检查栈容量。没有为每个标量无条件增加 pcall。
错误文本也在保护区写入，失败退回 false/nil；不得为漂亮错误信息再引入无保护分配。

scratch 清理只触及本转换创建的 pcall 输出/普通字段值，不触及调用者的 to-be-closed 槽位；转换不调用 lua_toclose。
Lua54 可能具有关闭语义，因此这不是声称任意 lua_settop 都不会执行用户代码。
read 保留入口栈，push 成功 +1，失败恢复入口 scratch；不取 Lua 错误对象的任意 tostring metamethod。

类型深度默认 32、每 record 导出字段上界 64；展开 typed 存储与根签名累加受 64 KiB 拒绝门槛约束，算术先检查剩余容量。
费用包含 expected<T, Failure>、optional<T>、两个独立 Failure 临时区、记录字段 Slots 和目标 T，并递归使用所选 policy。
签名还累计根 optional<Failure> 和各参数对齐余量；检查本层及合计容量，不能仅检查最大单参数。
这是保守的 typed 值存储上界，不是编译器机器栈字节承诺；C trampoline/编译器调用帧另有固定层级开销。
自定义规则如需要额外有限临时对象，以 storage/depth 声明；不允许隐藏无界递归或通用堆 fallback。
typed 对象按当前调用在有限 native frame 分配，不每 Entity 预留所有类型最大空间，不使用热路径无限 heap fallback。
既有 backend execution_depth_capacity 限制同时活动帧，嵌套调用拥有独立对象和根栈；用户规则不能 yield 或保存 Reader/Writer 跨挂起。
Lua table 本身仍可能正常分配，不能将“无额外 C++ 对象堆”宣传为全路径零分配。

失败路径按 `arg[N].field` 或 `result.field` 定位，固定 256 字节错误区有截断标记。
adapter 对自定义规则返回的未终止错误数组也执行有界复制，不把它当成无界 C 字符串。
错误对象仅在失败时构造；成功标量路径不再无条件初始化整块路径缓冲。
Reader 已知正索引和 field 的 base+1 不重复调用 DLL absolute；未知/负索引仍走原规范化操作。
这两项来自本轮初次测量和优化产物检查，不删除 raw 校验、构造回滚或调用资格检查。

官方错误行为核对：[Lua 5.4 §4.4](https://www.lua.org/manual/5.4/manual.html#4.4)、
[LuaJIT C++ 异常互操作](https://luajit.org/extensions.html)。实际 VM 构建和故障注入结果另外登记，不以文档替代运行证据。

## 3. 正式生成与迁移

`lux_script_lua_values` 是现有 codegen job/validation/projection 的薄入口，显式 SOURCES/LOGICAL_PATHS/RULE_HEADERS。
规则头使用 generator 既有 DEPENDS，不强制预包含目标自身导致重复解析；输出引用规则头，编译依赖也闭合。
validation 检查字段授权、冲突和范围；typed 编译检查实际成员访问、构造、叶规则与 layout，不按 offset 读写。
父类、union、指针图和未知模板不成为自动导出入口。

正式 Ability Lua 模板提供每参数/结果的方向、语义/布局和表示描述；backend 工厂与实际 typed read/provider/push 同时迁移。
现有两个 CollisionEvent fixture 共享一个正式值生成目标，使用 makeLuaValueOperation 薄 erased push。
LuaRecordMarshaller 类型/config/旧索引路径已删除，不留 alias/fallback；sol2 的对象/usertype 生成职责原样保留。
export record 参数在 prepare 固定操作；Event 在 prepareEvents 固定操作，resume 不重新按类型目录查找。
Execution/EventWaits/Timers/Ingress 不含任何转换字段、Lua table 或转换对象。

## 4. 验证登记

边界先行：实际 LuaJIT 独立 all 已通过 strict shape、const 构造、非默认构造对象、反向清理、push-only 和 OOM 恢复/初始化失败。
正式新测试：simulation_script_lua_value_boundary_test、simulation_script_lua_value_runtime_test 及解释执行变体。
最终生成、安装、增量、Lua54、性能与全量测试结果见同目录 SR-5 收口报告和原始归档。

已发现的中间试验：规则头强制预包含造成目标类型重复；MSVC 对 fold 内 requires 的处理；
const 返回 record 不满足既有 erased 输出赋值；源码行宽门禁。都保留原日志，修正测试/实现选择后重新构建。
这些不是运行时已复现的安全缺陷，也不能被“构建继续”掩盖。

## 5. 最终候选的支持边界

| 类型/方向 | 首批实现 |
|---|---|
| bool / i32 / u32 / float / double | strict 输入，既有数值范围；输出保留非有限浮点既有行为 |
| 有限 enum，底层 i32/u32 | 仅列出的枚举值，read/push 同样验证 |
| opt-in public aggregate record | raw table、字段改名、声明顺序、递归 typed 读写 |
| const 成员 | 聚合构造输入及输出读取；不能突破既有 Ability 不可赋值结果槽限制 |
| 隐藏字段/非聚合/非默认构造 | 需要显式 no-throw read 工厂，成功后才接管对象 |
| 自定义表示 | Policy + Rule 名称/版本；方向整体选择；所需方向缺失则冷期拒绝 |
| 普通 typed Ability | 参数 read / provider / 结果 push；结果失败不回滚 provider |
| export record CONST_REF 参数 | prepared erased push，由实际 typed rule 生成 |
| 已拥有的 Event payload | prepared 输出操作；结果/等待/恢复所有权不变 |
| async Ability | 保留原 native scalar；不接受自定义 scalar 表示或新 record/enum 协议 |
| union、bitfield、裸指针图、任意容器/对象绑定 | 不属于自动值转换；union 验证拒绝，无法取成员指针的 bitfield 编译拒绝 |

生成验证先完成再发布输出；typed 编译拒绝属于后续编译门禁，不伪称所有 C++ 不支持类型都由 parser 提前识别。
未导出的成员不恢复默认 table reader；本轮不提供隐含 default-construct-and-assign 构造策略。
相同语义类型的 custom push-only 也必须在实际 export/Event 与 Ability 输入目录一致，不能两种表示并存。

SR-4 与更早已量化 Event 债务继续有效：H0→SR4 Event 约 +11.14%；此前 Lua Update、prepare 等小幅差距保留原报告，
这些历史数值不替代本轮候选的新配对测量，不把缺少历史双向 record 能力的入口标为零成本。
