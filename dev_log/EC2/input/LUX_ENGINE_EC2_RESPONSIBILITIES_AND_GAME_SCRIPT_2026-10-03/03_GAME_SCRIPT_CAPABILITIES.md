# 03　游戏运行期能力与脚本投影：完整实施契约

## 1. 不是 Editor Scripting

本轮要支持的是游戏脚本读取运行资产、取得受控结果，并在合法运行区域中使用它。以下路径必须不含 Editor：

```text
脚本源/编译产物
    -> 现有 LuaScriptBackend / ScriptSystem
    -> 已准备 ScriptAbility 方法
    -> 运行期 provider
    -> AssetReadPort / loadAsset / 原 codec / Process
```

禁止通过 Editor 的 ProjectStorage 取得文件后端；禁止脚本修改项目 manifest、调用 Save As、创建 Pane、使用 History。Editor Play 可以向运行期提供它已装配的资产输入，但运行中的脚本看不到这个输入来自 Editor 还是 PLAYER。

源资产与 cooked 资产要明确：游戏默认读取宿主挂载的运行资产。不会默认得到 `.lux/editor`、作者源码、项目根目录或任意绝对路径。宿主确实需要开放调试数据时，用显式独立权限，不改变默认契约。

## 2. 当前已经有的基础，不重做

| 已读代码 | 能力 | 本轮要求 |
|---|---|---|
| AssetLoadSender.hpp | `loadAsset<ConcreteAsset>` 组合 AssetReadPort、CpuScheduler、TAssetSerDeser，返回拥有型结果 | IO/解码只有这条原算法或已有 codec registry 的对应正式算法，不复制在 Lua callback 中。[C18] |
| VfsAssetReadEndpoint.hpp | 有界读取，请求保活 endpoint；TaskScope 须活到接受工作结束 | 直接借用/组合，不自建磁盘线程或脚本文件后端。[C19] |
| ScriptAbility.hpp | QUERY/异步方法描述、语义参数、erased completion、值寿命标签 | 新方法沿这套契约，函数表在真实 ABI 边界不是违规。[C12] |
| ScriptAbilityInvocation.hpp | 外部异步结果先预留 awaitable；仅 void 或已声明且 trivially-copyable 类型 | 不能运输 shared_ptr/AssetBlob/任意 C++ 对象。[C13] |
| ScriptLocalAsync.hpp | 只有 ScriptTimers 铸造本地异步快捷入口 | 资产 IO 使用外部异步通道，不伪造 timer 授权。[C14] |
| ScriptSystem.hpp | 明确 execution region、stable point、lifecycle、外部完成与恢复预算 | 不从 Process 回调直接恢复 VM，不另建 pump。[C20] |
| DeferredScriptHost.hpp | 组件读取和受控 EcsCommandWriter，既有命令屏障 | 资产完成后写组件仍走原区域与命令，不传 Registry 给 worker。[C21] |
| ScriptAbilityLua.hpp | `makeLuaValueOperation`、bounded codec、Lua method projection | 新值通过同一生成/投影；不在 LuaBackend 中加资产类别 switch。[C17] |
| LuaScriptBackend.hpp | 预备方法/事件容量、value/ability 声明、统计和后端描述 | 正确声明类型与额度，不能降低准入来“支持任何类型”。[C16] |
| DelayAbility.hpp + lux_script_abilities | 真正已存在的能力声明/生成例子 | 新能力按同一构建接入，Delay 保持原实现。[C22,C23] |

本次没有穷尽读取所有游戏能力声明。R0 先检索是否已有资产能力 provider；有等价实现即完善并复用，不按本文件的目标名再造一个。

## 3. 原生接口与脚本接口如何共享

### 3.1 原生 C++ 层

继续支持现有直接调用（已有 API）：

```cpp
auto work = process::asset_loading::loadAsset<asset::SkeletonAsset>(
    reads,
    execution.cpu(),
    id,
    limits,
    stop
);
```

这是 sender，不是已完成资产，也不能直接在当前 Hook 中同步等待。C++ 宿主通过既有 Task/TaskScope 消费结果。

脚本 provider 调用同一读取与解码实现。它额外负责的是**语言不可直接表达的调用寿命、额度、稳定句柄和结果运输**，不是第二份资产加载、缓存、目录或序列化实现。

### 3.2 本轮脚本必须提供的能力

下表是**目标签名与语义**，具体用户语法须沿现有生成器，不是已经存在的 API。最终交付包含真正可运行的 `.lua`，不能照抄此伪接口而不接生成链。

| 操作 | 语义类别 | 约束 |
|---|---|---|
| `readAsset(id)` | ASYNC | 按原 AssetReadPort 读完整资产镜像，返回可运输 outcome；不等于 GPU resident。 |
| `describeAsset(handle)` | QUERY | 只读已取得镜像/类型/尺寸/来源标识，不发起 IO；未知元数据明确缺失。 |
| `copyAssetBytes(handle, offset, count)` 或等价有界访问 | QUERY | 仅复制已持有缓冲的指定范围到受控 Lua 值，检查溢出和最大返回字节；不是每帧全量 tostring。 |
| `releaseAsset(handle)` | COMMAND/同步资源操作 | 释放该脚本作用域的原生结果持有；不卸载其他使用者的资产，也不等待 GPU。 |
| 一种正式 typed 读取，如 Skeleton | ASYNC + QUERY | 用原 SkeletonAsset codec 返回 typed 句柄；可读骨骼数/有限字段，不复制骨骼数据类型或 codec。 |
| 既有 DelayAbility + 一种正式组件读/写 | 复用验证 | 与资产读取组合，证明有 query、受控 command、awaitable 三种调用，而非资产专用 VM 特例。 |

“元数据查询”只对 provider 已有固定目录/已加载结果成立。若底层没有廉价目录 API，不新增一个假 `exists()`，也不在同步方法中偷偷读磁盘。按 AssetId 读取失败即可准确报告；虚拟路径解析若提供，必须复用原 VFS、在已知快照上进行，并区别缺失与不可访问。

通用 readAsset 提供镜像；typed accessor 可由 Skeleton 的独立能力贡献提供。通用 VM 与资产读取核不得 hardcode Skeleton/Material/Model。没有已存在动态 codec registry 时，先用静态 `loadAsset<T>` 的领域投影；不为本轮构造巨型万能解码器。

### 3.3 操作与结果的命名

- read 成功只代表取得字节；decode 成功代表取得合法 CPU 资产；renderer 接纳/驻留/GPU 完成是其他事实。
- request/start 表示接纳，complete 表示该请求终态，release 表示释放本作用域引用。
- 数据对象可保留 size/find/query 等自身观察方法；它不在 getter 内读取文件或调度任务。
- 原生错误按原类型保留；脚本边界映射为稳定的 domain/code/必要固定信息，不使用日志文案判断。

## 4. 结果为什么需要句柄，而不是 SharedBytes 直接返回

当前外部异步 resume 通道的静态要求不能安全传递带引用计数和析构的 C++ 值。[C13]

目标：

```text
原生 AssetBlob / shared_ptr<const T>
    -> 原生脚本结果 owner 保活
    -> 小型 ScriptAssetHandle（域 + slot + generation）
    -> 现有可运输结果槽
    -> Lua 中有明确语义的只读句柄值
```

### 4.1 句柄的完整契约

- 全部身份来自原生 owner。脚本不得提供 context 指针、scope 指针或伪造权限声明。
- 不把指针转整数，不把 AssetId/uint64 一律转 Lua double；128-bit AssetId 保持原精度。
- handle 至少区分所属 provider/作用域与槽代次；同槽复用后旧 handle 拒绝。
- scope/完整实例身份使用既有身份机制；若没有合适分配器，新增仅负责这一真实域的冷路径分配，不用每 DLL header static 伪造全局唯一。
- Lua 表示优先沿已有 `LuaValueOperation/TLuaValueCodec` 的 typed 值机制。纯 table 无法被当作可信 native handle；decode 每次检查 type/size/域/代次，不只检查字段存在。
- typed 句柄还验证对应 AssetTypeId/语义表示版本，类型不匹配不能退化为 raw reinterpret_cast。

### 4.2 结果表不是另一个资产缓存

它只保活当前调用获得的原生结果并将句柄关联到它；不重做依赖加载、路径解析、全局去重或后台线程。

同一 immutable bytes/asset 可被多个原生 owner 共享。Lua 的 token 本身不复制大 buffer。逻辑保留字节计入本作用域预算，不能仅因底层共享就让脚本绕过额度。

### 4.3 明确采用 scope-owned token

本轮默认语义：结果由原生脚本实例作用域拥有，handle 是对此记录的稳定引用，不是拷贝一次就创建一份独立释放责任的 shared_ptr。

- 同一 handle 的 Lua 赋值/拷贝是别名。
- 显式 release 使该记录的所有旧别名失效；重复 release 返回明确已失效/已释放结果，不发生第二次释放。
- Lua `__gc` 不直接取消其他别名的所有权，不调用阻塞 IO/业务/Registry；不把 GC 时机当作容量和关闭协议。
- 实例关闭必须撤销其结果域，并释放所有已交付但未显式释放的记录。
- 跨实例共享必须通过正式重新获取或明确 transfer/retain 协议；本轮不暗中支持 transferable handle。

这使容量和脚本销毁行为可确定。需要一般共享脚本对象引用计数时另作明确需求，不能偷偷改变此合同。

### 4.4 字节额度的层次必须诚实

已读 `ReadAssetImage` 只有 AssetId，VfsAssetReadEndpoint 配置只有 request_capacity，执行段调用 `vfs.open(id)`。[C18,C19,C27] 这**不足以证明**单请求读取前就按脚本 max_bytes 限制了下游临时分配。

R5 必须沿真实 provider 核对：镜像大小在何时确定、是否先于分配/解压验证、固定挂载版本怎样保持。脚本结果保留预算、codec decode 限额、后端瞬时内存和整个进程 RSS 分别报告。

有现成 provider 限额即复用；暴露的读取需要新的字节上界而旧协议不能表达时，应在原 ReadAssetImage/endpoint/provider 的正式操作中补准确限额并迁移消费者，不新增 Lua 专用文件读取。不允许仅在巨量字节已分配后做 size 判断，就宣称“全链路严格内存上限”。无须借此建设进程级 OOM 恢复系统。

### 4.5 目标类型示意（非现有可编译 API）

```cpp
// 是运输身份，不拥有 Asset，不包含 C++ 指针。
struct ScriptAssetHandle final
{
    std::uint64_t domain{};
    std::uint32_t slot{};
    std::uint32_t generation{};
};

// 实际状态和语义 ID 用正式声明生成；下面只表达关联。
struct ScriptAssetReadOutcome final
{
    EScriptAssetReadStatus status{};
    ScriptAssetHandle handle{};
    std::uint32_t error_domain{};
    std::uint32_t error_code{};
};
```

这不是新数据袋任意组合的许可。结果由 native factory 构造，成功/失败关系校验；进入 Lua 为只读观察。实际 ABI 必须验证语义 ID、字段布局/对齐、保留字段初始化和版本，不以 `sizeof` 相同认定类型相同。

范围返回值使用已存在的 bounded 语义值或有限 `AssetByteChunk` 投影；不无限字符串化整个资产。除 transport 必须的小值，域内继续使用原 AssetId/AssetTypeId，不再复制一套游戏 UUID。

## 5. 实例作用域：不能从脚本传入一个 caller_id 解决

这是 R0/R5 必须作出的具体核验，不能假定已有能力全部满足。

当前公开 `ScriptApiCapabilityPublication` 提供 provider context/dispatch，`ScriptSystem::create` 接收能力与后端数组；已读接口不足以证明任意新资产 provider 都能取得可靠的逐实例释放通知。[C15,C20]

实施顺序：

1. 读取当前 ScriptPreparer/ScriptInstances/ScriptBackend 的完整相关调用；定位实际 ScriptInstanceId、prepared method、能力 lease 以及实例撤销位置。
2. 若已有逐实例 provider 准备/释放能力，直接用它产生 `ScriptAssetScope`（目标名）。
3. 若没有，在**原能力准备/实例生命周期**处补一个窄的 scope prepare/revoke 关系：由真正的 native 实例身份铸造 context，保存到原 prepared binding，实例退役先标记 scope stopping，最后等接受工作回收。不是全局 ScriptScopeRegistry，也不是第二个 ScriptSystem。
4. 固定能力描述和生成 traits 不变成每帧数据。只在冷准备时绑定当前 scope，热调用直接用已准备 typed/erased method。
5. 不使用当前焦点、全局 CurrentScript、TLS 裸状态、Lua 可写字段或整数传参判断真实调用者。
6. 同域多个脚本只能访问各自句柄；旧 ScriptInstanceId 的结果不投给新代际实例。

这项小扩展可能改变 script 的内部准备合同或相关 ABI，必须记录精确影响并更新合法消费者；**不得为避免改动原模块而使用不可靠的用户态 caller 参数**。Editor V8 与 Script ABI 是两套边界，不因名称相似一并升级；真实公共变化才升级对应指纹/版本。

## 6. 异步接纳、结果和清理的完整次序

```text
脚本调用已准备的方法
 -> 检查当前能力与输入
 -> 原 awaitable 预算接纳（已有机制）
 -> provider 为本请求预留结果/字节/记录额度
 -> 复制或持有稳定输入，启动原 TaskScope/loadAsset
 -> IO/CPU 完成，回调不进入 Lua、不写 ECS
 -> 原完成入口保存运输结果或进行纯保管步骤
 -> ScriptSystem 的既有安全点采用并恢复
 -> 脚本读取 outcome/handle
 -> 显式 release 或实例结束清理
```

### 6.1 两种失败不能混淆

**调用/协议无法接纳**：能力缺失、schema 不符、方法输入错误、awaitable/请求预算不足、实例 stopping。通过现有 start/call error 返回；不得先启动工作再说拒绝。

**已接纳的业务失败**：资产不存在、格式不匹配、读取 IO 错误、取消、预算被具体输入超出。这些应当能以脚本可检查的 `AssetReadOutcome`（目标固定值）表达；普通“文件没找到”不应只能触发 ScriptSystem instance fault。

目标 outcome 是 `status + optional-valid handle + 固定错误域/码` 的有约束结果表示。native 工厂构造、Lua 只读观察；成功必有合法 handle，失败不含有效 handle。它必须满足当前语义/字节运输要求。不要将 std::variant<std::string,...> 直接 memcpy 作为所谓 trivially copyable result。

协议自身损坏、底层异步通道失效与业务 NOT_FOUND 分开记录。保留原完整 native 错误用于宿主诊断；脚本传输不携带 std::any、外部异常对象或临时字符串视图。

### 6.2 预留与交付失败

- 在请求获准前预留完成保管能力；接受后不能因为 UI BUSY 或临时执行期状态而丢完成。
- 允许底层同步完成，但仅入原完成路径，不能在 start 返回前任意重入脚本。
- 结果准备后若 completion 已 stale，立即在合法 native 清理位置释放未交付句柄/额度，不留下孤儿 slot。
- completion 已接受但实例在恢复前销毁时，scope 撤销必须回收结果。只在发送前检查 `active()` 不足以证明这一时序。
- 生命周期取消停止用户交付，但原 IO/task 的所有权和最后清理仍需要完整结束。
- cancel/release 不调用其他作用域 TaskScope.stop；不得停止共用 provider 的全部请求来取消单脚本。

### 6.3 owner 线程与销毁顺序

给出最终实际顺序图，至少包含：原资产 endpoint、TaskScope、script scope、awaitable、Lua coroutine、typed result、code owner。

- VM 和 ScriptSystem 只在其 owner/lifecycle 位置触达。
- 需要异步的 native scope 记录由已存在 TaskScope/完成引用保活；停止后业务能力撤销，但不能提前销毁 worker 要回写的记录。
- 服务/endpoint 整体 close 由宿主在实例退休和完成结清后进行；借用 endpoint 的作用域不擅自关闭公共 endpoint。
- 插件提供的 codec/value/deleter/code pin 必须覆盖 result 的最后析构，含移动赋值和失效结果清理。游戏侧使用原 engine/plugin 的代码保活，不得为方便引用 `editor::contracts::CodeLease`。
- 原 Scene/Simulation 的命令屏障仍唯一。数据已加载不授权 worker 创建实体。

## 7. 三种能力形式分别实现

### QUERY

只读取当前合法快照或已持有结果，不 IO、不等待、不分配大缓冲。必要的有界值拷贝明确声明。读取组件返回值/只在本 step 有效的借用；跨 await 不能保存组件地址。

### COMMAND

修改组件通过已有 DeferredScriptHost/EcsCommandWriter 的合法执行区域。回调不直接在 EnTT observer 内破坏 pool。命令接纳与屏障实际执行分别报告，不能把 true 当成世界已经更新。

### ASYNC

使用 ScriptAbility 外部完成、原 Process 和现有 resume 机制。不得把任意 disk IO 放到 local timer 特权路径，不自建 Lua promise 系统/线程队列。

原生 static/C++ 脚本与 Lua 投影使用同一方法语义和错误映射。不要给 Lua 每个操作另写一份验证、文件解析和状态机。

## 8. Lua C 边界

仓库已有 `LuaBoundary.h`：typed worker 正常返回、C++ 临时值销毁后，由 C boundary 处理 RETURN/SUSPEND/ERROR。[C26]

所有新增绑定继续走这个边界与既有生成器。不能在持有 RAII scope/unique_ptr/CodeLease 的 C++ 栈上直接 `lua_error`/yield，不能靠普通 C++ catch 捕获 Lua 的非局部跳转。Lua 5.5 标准手册说明错误/yield 与 C continuation 的约束；本仓库的实际补丁和边界实现是落地依据。[W01]

- 注册/marshal 不手写另一套 metatable 管理器，有现成 bounded TLuaValueCodec 就复用。
- type operation 的 size/alignment/representation/version 必须匹配；错误签名在准备时拒绝。
- 业务 expected 错误转换一次，不按日志字符串二次推断。
- 不把 raw native pointer 放入 lightuserdata 当可自由持久化的能力。
- 环境默认不因新资产功能开放任意 `io/os/debug/package.loadlib`；沿原宿主配置明确白名单。没有审计 VM 全部攻击面，不宣称安全沙箱已认证。

## 9. 最小可工作的交付样例

同一组项目运行包和用户逻辑分别在独立 PLAYER 与 Editor Run 执行：

1. 请求一个真实存在的 SkeletonAsset 或另一个现有 typed CPU 资产；原 codec 实际读取。
2. 读取有限元信息，检查完整 ID/类型/字段与 C++ 对照一致。
3. 等待一次 Delay（原机制），再次读取合法句柄。
4. 在合法 step 中读取/修改一个已显式开放的实际组件，通过原命令屏障观察结果。
5. 释放资产，再访问旧句柄得到准确失效；退出无悬挂任务/awaitable/结果记录。
6. 同一脚本请求缺失资产时，能够查看业务失败并继续执行；不以单个 false/空日志代替。

另有独立无 SceneRender/GPU 的 C++ 消费者使用同一 asset read/codec 能力。不得要求为了读取 Skeleton 注册材质编译器或安装整个 Editor。

样例和能力是实际产品模块的消费者，不是测试程序中的另一套加载器。最终提供真实 `.lua`、生成声明、宿主接线、包输入和期望输出。

## 10. 不在本轮冒称完成

没有 Lua 作者文档/IDE、完整 script attachment 产品体验、断点调试、任意动态插件热卸载、全场景/物理/音频 API、任意 C++ 类型零成本反射绑定、可靠沙箱认证、Android/Linux 实测等新声明。

可扩展性证明是“一个资产能力和既有另一能力通过统一契约独立组合，新增普通能力不修改通用 VM”，不是“从此任何函数都能无成本脚本化”。
