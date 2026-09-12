# NA1 热路径操作收口

工作起点为 `96eef6246c96d86d5a5f51d2ca7c7a2ce8331d25`，生产对照仍是
`4a8812e9813fe90b241dba29e3e668e4a4239b26` 及已保存的 B 镜像。
本文件不替换 [原结果](RESULT.zh-CN.md)、[P4 采样](P4-VTUNE-FOLLOWUP.zh-CN.md)
或 [32 项审计](P4-OPERATION-AUDIT.zh-CN.md) 的身份和测量。

## 正式调用入口

CppStatic 作者在类型上声明 `inline static constexpr std::array SyncStepShapes{...}`，
元素由 `scriptSyncStepShape<Signature>()` 产生。生成 contract 引用这些只读、静态寿命的形状。
组合准备按实际 Lua export 比对参数的规范名、类型 ID、ABI kind、size、alignment、pass 和返回形状；
CppStatic 子对象创建再校验发布的 shape 属于该 contract，且实际签名匹配。
没有声明或形状不匹配返回 `EXECUTABLE_CONTRACT_MISMATCH`，不回退到旧的逐字段 ABI 校验。
该声明必须与调用模板和生成 contract 属于同一 C++ executable/module；跨模块搬运调用上下文不是授权入口。

热入口仍检查 active step、完整实例身份、publication、ordinal 和实际调用模板的 shape 身份。
模板只能传其真实 C++ 值的地址；它不再构造 value_slot、call_frame、passes 或逐参数取模。
错误 ordinal 和不同类型的动态调用仍分别返回 `-32002`、`-32003`。
初始资格由 C++ 桥捕获并校验一次，传给 Lua；转换/栈准备之后、Lua 执行之前再次检查原资格。
Lua 返回并恢复 ExecutionScope 后，C++ 桥检查原 publication 和原资格，再向任务发布 expected 结果。
非零 backend 错误保留优先级，不被后置撤权错误吞掉。Lua 只写调用方私有的 scalar 暂存，
失败时不把暂存返回给任务，也不声称回滚已经执行的业务。

## Lua 边界

冷期选择 scope、void/scalar 结果及参数 push/read 操作；准备失败释放 prepared method，计划数组只在完整构造后提交。
计划由 function binding 所有并在其释放时销毁，不跨 invocation 共享可写 request。

纯内建 scalar 参数使用主 Lua 栈上的直接 `lua_pcall`，移除 executeSyncStep C trampoline 和 request。
外层 checkstack 返回失败，不通过 longjmp 退出；已有额度内的 raw registry lookup、light C function push、
number/bool push 不分配。checkstack 可能调用 allocator，所以执行 Lua 前仍重验原资格。
错误 handler 用同一私有 traceback C 函数，保留错误追踪，省去 handler registry lookup。
int32/uint32 仍推送 Lua Number，未改成 Lua integer。

record/custom 参数使用受保护 C callback；callback 自己建立额度，依次执行转换，
不提前读取后续字段。保留 converter 的拥有型临时对象 frame 与 `lua_settop(vm, 1)` 的 TBC 清理时点。
真正的 Lua 返回值仍检查类型/range。两条路径都保存/恢复原 main-stack base 与嵌套 ExecutionScope，
不支持从同步步骤挂起，不增加恢复点。直接路径少一个引擎内部 C 调用帧，debug hook 看到的内部调用栈会相应变化。

## 共享等待路径

- 等待的期望类型继续每次写入，但规范名 hash 使用 constexpr 值；同一任务不同类型的等待不共用错误的类型状态。
- `prepareResume` 验证 READY/type/size 后，只保存本次同步 resume 窗口的 data 借用；私有 typed awaiter 直接复制业务值。
  结果仍由原 packet owner 保管，原 awaitable 逻辑槽仍在用户恢复前归还。
- CppStatic 每实例按 import 顺序保存 Event admission，冷期完成 local→actual 映射；不同 contract 和越界 local 继续拒绝。
  固定 backing 与 Ability 共用实例 binding block 的分配/回收目录，统计计入 Event handle 数组。
- prepared Event 的结果布局由 Preparer 证明；任意外部结果仍走 checked 描述入口，共用私有动态接纳内核。
  source preflight→awaitable→source commit、Timer 的接纳优先级、OOM、pin、queue、frontier 与预算均保持。
- Event waiter 插入成功后直接使用 dense 尾项，在没有用户代码的登记区间完成链接；不跨插入/dispatch 保存该指针。
  StableSlotMap 结果的 insert→find 仍保留，固定 lux-cxx 尚无返回所构造记录的接口，本轮不改依赖或绕过其封装。

## hash 与范围

固定 lux-cxx 已提供 `algorithm::TFnv1a`（consteval）和 `algorithm::fnv1a`（constexpr）。
脚本 identity 使用现有 semantic canonical-name FNV 规则，其 unsigned-byte 和零值处理不等同于直接换一个 C++ type hash。
本轮通过 consteval 形状构造与 constexpr 等待类型强制使用既有稳定 ID 的编译期结果，未新增 hash 实现。
方法身份、真正输入、错误、取消、容量和回收保护不因这项优化删除。

未选择另建 closure/upvalue roots、共享全局资格缓存、改 StepContext ops 表或关闭已有公开统计；
未确认的多余 move/错误 symbol load 不凭源码行数认定为已消除成本。没有新调度器、结果池、thread 池或旧 ABI fallback。

## 资格状态

修改工作树的全量构建和受影响测试：Toolchain 69/69，Developer 88/88。
新增真实 runtime CASE 已覆盖冷期缺失/错误 shape、flat void 错误/yield/Event/TBC 与合法 recovery。
原 record、重入撤权、OOM、返回值拒绝、身份、生命周期与共享协议断言继续通过。
最终 clean clone 21d10601 的 Toolchain 110/110、Developer 127/127、VM 4/4、16 个安装消费者、
13 类原增量、5 类组合增量及 3 条迁址均已执行；机器码、成本、资源与限制见
[实际结果](HOTPATH-RESULT.zh-CN.md)。上述早期工作树结果不冒充最终资格身份。
