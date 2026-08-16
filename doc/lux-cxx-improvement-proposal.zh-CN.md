# lux-cxx 改进建议与跨项目基础语义迁移计划

> 审计日期：2026-08-15
>
> 审计范围：同级 `lux-cxx`、`lux-engine`、`lux-robotics`、
> `lux-communication` 当前工作树。
>
> 本文只讨论 lux-cxx 的职责、API、质量门禁和迁移计划。lux-engine 的
> Extension、modules、engine、ecs 边界另见
> [模块、插件与工程边界审计](module-extension-engine-boundary-audit.zh-CN.md)。

## 1. 结论摘要

lux-cxx 当前最需要的不是横向增加更多“大框架”，而是成为 lux 项目簇唯一可信的
**跨项目 C++ 基础语义层**。

建议未来两到三个版本集中完成五件事：

1. 统一身份类型：进程内类型、强类型整数、稳定名字、UUID、临时 slot、schema、
   content digest 各自使用不同类型；
2. 建立真正的 `core` component，把 `expected`、callable、contracts、fixed text、
   checked arithmetic 等从 `compile_time` 或各下游重复实现中收敛进来；
3. 统一 owning bytes、digest、时间域、序列化底座和有界通道的状态语义；
4. 强化现有 SlotMap、SmallVector、queue、ObjectPool、reflection、serialization，
   而不是另建同名容器和异步运行时；
5. 建立 MSVC/GCC/Clang、Debug/RelWithDebInfo、sanitizer、DLL ABI、安装后 consumer、
   fuzz/golden-vector 的基础库质量矩阵。

明确不做：

- 不新增 ECS；
- 不新增通用事件总线；
- 不新增第二套 coroutine/stdexec/Asio runtime；
- 不新增万能线程池；
- 不统一 engine 与 robotics 的数学体系；
- 不把 Asset、Scene、Render、Entity、Extension 等引擎词汇放进 lux-cxx；
- 不允许 compiler type hash 进入文件或网络。

建议的最高优先级是：

```text
P0-1  修复所有持久化 type_hash，首先是 LFGR v2
P0-2  建立 identity/core canonical API
P0-3  打开并补齐现有基础组件的质量门禁
P1    bytes/digest/time/binary serialization/container/channel 收敛
P2    diagnostics/ABI/reflection backend 与长期兼容策略
```

---

## 2. 当前状态与真实需求

### 2.1 使用面量级

按包含相关表达式的 C++ 文件数粗略统计：

| 搜索项 | lux-cxx | lux-engine | lux-robotics | lux-communication |
|---|---:|---:|---:|---:|
| expected/unexpected | 8 | 306 | 49 | 0 |
| move_only_function | 4 | 57 | 2 | 0 |
| std::chrono | 10 | 83 | 79 | 44 |
| 声明某种 Id/Handle/Key | 11 | 92 | 30 | 2 |

统计受命名和模板风格影响，不能当作精确 KPI，但足以证明下面几项是跨项目共同需求：

- fallible result；
- owning/non-owning callable；
- 多 clock domain 时间；
- 强类型身份；
- generation handle；
- 有界队列与关闭；
- wire format 与 digest。

### 2.2 lux-cxx 已经具备的能力

改进计划必须建立在现状上，不能假设它们不存在：

| 现有能力 | 位置 | 当前评价 |
|---|---|---|
| expected 门面 | `compile_time/.../expected.hpp` | 已选择 C++23 `std::expected` 或回退实现 |
| move-only callable | `compile_time/.../move_only_function.hpp` | 自实现；空调用仍依赖 assert |
| inplace callable | `compile_time/.../inplace_move_only_function.hpp` | 可保留独立语义，需补 contract/测试 |
| 类型名/hash | `compile_time/.../type_info.hpp` | 已明确禁止持久化，但下游仍违反 |
| SlotKey/SlotMap | `container/.../SlotMap.hpp` | dense、generation-safe；地址不稳定 |
| SmallVector | `container/.../SmallVector.hpp` | 已有 SBO；当前不支持 allocator |
| ObjectPool | `memory/.../ObjectPool.hpp` | 需并发、失败和生命周期验证 |
| SPSC queue | `concurrent/.../LockFreeQueue.hpp` | 固定容量、有 close、非阻塞状态 |
| Blocking queue | `concurrent/.../BlockingQueue.hpp` | 有 close/timeout；状态 API 尚未统一 |
| ThreadPool/Timer | `concurrent/` | 不应再建第二套，只做定位与质量审计 |
| signal/event | `event/` | 不应演化为跨项目业务总线 |
| SHA-256 | `algorithm/.../sha256.hpp` | 算法已有；缺统一 digest value/parse/format |
| serialization | `serialization/` | 偏 reflection/archive；缺轻量 binary foundation |
| reflection | `reflection/` | Clang generator/runtime 已存在，需稳定 metadata IR |

因此本计划不是“新增十几个模块”，而是：

- 重新命名和分层已有 API；
- 补缺失语义；
- 合并下游重复实现；
- 删除不可靠或未验证实现；
- 建立跨项目兼容门禁。

### 2.3 当前最重要的重复实现

#### 稳定名字 ID

- lux-engine：`BasicOwnedStableId<Tag>`；
- lux-robotics：`StableId<Tag>`；
- authoring：`BasicStableNameId<Tag>`。

它们在 owning、borrowed、hash、canonical validation 上语义不同。

#### UUID wrapper

lux-engine 至少有两份 `BasicUuid<Tag>`：

- EntityScene identifiers；
- Authoring World identifiers。

#### Digest

- lux-cxx 有 `Sha256Digest` 数组和 SHA-256 算法；
- lux-engine 有 `ContentDigest256`/`Sha256Hasher`；
- lux-robotics 有 `Sha256Digest` 和一份局部 SHA 实现。

#### Callable/fixed diagnostics

- lux-cxx 有 owning move-only callable；
- lux-robotics 有 two-word non-owning `Delegate`；
- lux-robotics 有 `FixedText`；
- engine 存在多个局部 ScopeGuard/EnumFlags。

#### Byte/wire 基础

- lux-engine 有 `SharedBytes`、ByteIO、NameTable、多个手写 bounded codec；
- lux-cxx serialization 更偏 reflection/archive；
- communication 又有自己的 wire/ring framing。

这些重复正是适合收敛的对象。

---

## 3. lux-cxx 的职责边界

### 3.1 应负责的内容

lux-cxx 应只提供不认识具体业务领域的 C++ 原语：

- result、callable、scope、contract；
- 强类型 identity 与 generation key；
- owning bytes、digest、checked arithmetic；
- clock-domain-aware time；
- 通用容器和 memory resource；
- 有界队列与 admission/close 状态原语；
- binary encoding 基础；
- reflection metadata IR 与 frontend/runtime 分界；
- structured diagnostic value；
- deterministic test support。

### 3.2 不应负责的内容

以下内容即使多个项目都使用，也不应进入 lux-cxx：

- EntityRegistry、Schedule、component catalog；
- DomainEvents、ROS-like topic、gameplay events；
- AsyncRuntime、coordinator、executor graph；
- AssetManager、Pak、EntityScene、Material、Mesh；
- Render session、GPU handle、Vulkan；
- ExtensionModuleDescriptor、engine registrar；
- SLAM、odometry、navigation、physics contract；
- 文件监控、窗口、动态库加载器等 OS backend；
- Eigen/GLM/李群统一数学层。

### 3.3 接受新能力的判据

一个新能力只有同时满足以下条件才进入 lux-cxx：

1. 至少两个独立产品存在真实 consumer；
2. API 不出现 consumer 的领域名词；
3. 无需 consumer 的 composition root 才能正确工作；
4. ownership、thread、failure、close 语义可独立描述；
5. 可独立测试和安装；
6. 不会制造第二个已有框架。

如果只满足“代码看起来通用”，先留在原项目，等第二个 consumer 出现后再抽取。

---

## 4. 身份模型

### 4.1 必须区分的七类身份

```text
TypeToken<T>                   进程/单次构建内 C++ 类型身份
StrongId<Tag, Rep>             强类型数值身份
StableNameId<Tag>              canonical 名字 + stable hash
UuidId<Tag>                    128-bit 持久对象身份
SlotKey<Tag, Index, Gen>       进程内临时对象句柄
SchemaId<Tag> + SchemaVersion  文件/网络格式身份
ContentId<Tag, Digest>         内容寻址身份
```

七种类型必须不可隐式互换。名字叫 ID 并不意味着语义相同。

### 4.2 TypeToken

用途：

- 同一进程的 type map；
- callback/registry dispatch；
- telemetry 中辅助显示 C++ 类型；
- 同一构建内的碰撞诊断。

推荐形状：

```cpp
struct TypeTokenView final
{
    std::uint64_t hash;
    std::string_view name;
};

template <class T>
consteval TypeTokenView typeToken() noexcept;
```

约束：

- hash 来自 compiler spelling 没问题，但相等必须同时比较 name；
- 只允许 borrowed static name；
- 不提供 serializer；
- 不提供稳定字符串的假承诺；
- public 文档重复强调 per-build；
- architecture gate 禁止 wire/file writer 使用 `type_hash`/`type_name`。

### 4.3 StrongId

推荐形状：

```cpp
template <class Tag, std::unsigned_integral Rep>
class StrongId final
{
public:
    static constexpr StrongId invalid() noexcept;
    static constexpr StrongId fromValue(Rep) noexcept;
    constexpr Rep value() const noexcept;
    constexpr bool valid() const noexcept;
};
```

设计要求：

- 不隐式转换成 Rep；
- invalid sentinel 由 traits 明确；
- 支持 hash/order；
- wire width 固定且由 codec 明确；
- arithmetic 默认禁止；
- 若确需单调 sequence，使用单独 `Sequence<Tag, Rep>`。

### 4.4 StableNameId

需要 owning 和 view 两种：

```cpp
template <class Tag>
class StableNameId;

template <class Tag>
struct StableNameIdView;
```

建议字段：

```text
owned: std::string name + uint64 stable_hash
view:  string_view name + uint64 stable_hash
```

规则：

- hash 使用明确、冻结、带测试向量的算法；
- 比较必须是 `(hash, full_name)`；
- 同 hash 不同 name 返回 collision，不是 equal；
- canonical validator 通过 policy 注入；
- lux-cxx 只提供 ASCII/UTF-8 基础 validator 积木；
- reverse-domain、路径、topic 等领域规则由 consumer 定义；
- decoder 必须重算 hash 验证，不能信任磁盘值。

### 4.5 UuidId

推荐提供与 UUID backend 解耦的 16-byte value：

```cpp
template <class Tag>
class UuidId final
{
public:
    std::array<std::byte, 16> bytes() const noexcept;
    bool nil() const noexcept;
};
```

如果继续使用 stduuid，应只在 adapter/constructor 层暴露，避免所有 consumer 公开依赖它。

必须规定：

- wire byte order；
- canonical text format；
- nil 是否允许；
- parse error；
- random/name-based UUID 的生成位置不属于 value type。

### 4.6 SlotKey 与容器身份

现有 `SlotKey` 可保留，重点补齐 contract：

- `index` 与 `generation` 位宽；
- generation 最大值时 retire slot，禁止 wrap ABA；
- null 表示；
- stale lookup 返回状态而非只 assert；
- serialized API 对 SlotKey 默认禁用；
- container move 后 key 的 owner domain 是否变化；
- debug owner-cookie 是否仅测试启用。

Dense SlotMap 与 StableSlotMap 应是两个容器：

- `SlotMap`：cache-friendly dense iteration，erase 可移动元素；
- `StableSlotMap`：地址稳定，元素独立 node/arena，迭代局部性较弱。

不要做几十个 policy 参数的统一模板。

### 4.7 SchemaId 与 SchemaVersion

推荐：

```cpp
using SchemaId = StableNameId<SchemaTag>;

struct SchemaVersion final
{
    std::uint16_t major;
    std::uint16_t minor;
};
```

规则：

- major 变化表示 reader 不能直接接受；
- minor 表示 additive/backward-compatible 能力；
- field 使用稳定字段名或显式 field id；
- schema version 不能等同 library/package version；
- migration registry 属于上层 domain；
- lux-cxx 只提供 migration step/result 原语。

### 4.8 ContentId 与 Digest

推荐：

```cpp
struct Digest256 final
{
    std::array<std::byte, 32> bytes;
};

template <class Tag, class Digest = Digest256>
class ContentId final;
```

规则：

- digest 是内容身份，不是对象身份；
- 空 digest 的语义明确；
- parse/format 在独立 header；
- hashing algorithm 与 domain separation 由 ContentId policy 决定；
- `sha256(payload)` 与 `sha256(domain || schema || payload)` 是不同身份；
- constant-time equality 只给安全 consumer，普通内容索引无需强制承担成本。

### 4.9 P0：禁止持久化 compiler type hash

lux-cxx `type_info.hpp` 已正确声明 `type_hash<T>()` 不是 ABI-stable；但 lux-engine 的 LFGR
v2 仍把它写入变量、节点参数和常量 payload。

必须建立两层防护：

1. 修格式：LFGR v3 使用稳定 scalar schema；
2. 防回归：codec/wire 路径禁止调用 compiler type identity。

LFGR v3 推荐 scalar ID：

```text
lux.scalar.bool/v1
lux.scalar.i8/v1
lux.scalar.u8/v1
lux.scalar.i16/v1
lux.scalar.u16/v1
lux.scalar.i32/v1
lux.scalar.u32/v1
lux.scalar.i64/v1
lux.scalar.u64/v1
lux.scalar.f32/v1
lux.scalar.f64/v1
lux.scalar.utf8/v1
```

迁移要求：

- v2 reader 保留为 migration-only；
- v3 writer 不再写 type_hash；
- 旧 reader 不要求理解 v3；
- golden vectors 固定；
- MSVC/GCC/Clang 各自产同一 v3 bytes；
- Android/Windows round-trip 后 exact bytes 相同。

---

## 5. Core component 重构

### 5.1 目标目录

```text
lux-cxx/core/include/lux/cxx/core/
  expected.hpp
  function_ref.hpp
  move_only_function.hpp
  inplace_function.hpp
  delegate.hpp
  strong_id.hpp
  stable_name_id.hpp
  uuid_id.hpp
  schema_version.hpp
  fixed_text.hpp
  enum_flags.hpp
  scope_exit.hpp
  contracts.hpp
  checked_arithmetic.hpp
  source_location.hpp
```

`compile_time` 只保留真正需要编译期计算的能力：type traits、fixed string、type spelling、
consteval utilities。`expected` 和 runtime callable 不再归类为 compile-time。

### 5.2 expected

现有 feature-test 选择逻辑是正确方向：

- C++23：alias `std::expected`；
- C++20：使用兼容实现。

后续工作：

- canonical include 移到 `core/expected.hpp`；
- C++20/C++23 编译与行为测试共用一份 test corpus；
- 锁住 `expected<void,E>`、move-only value/error、monadic API 可用范围；
- 文档标明哪些 API 在 fallback 中可用；
- 禁止下游直接依赖 fallback namespace；
- 不新增万能 Error 基类。

迁移策略可以先保留旧 include 的短期 deprecated forwarding，但必须带 removal version；
若 lux 项目簇可以一次同步升级，则更推荐 major hard cut。

### 5.3 move_only_function

目标：

- C++23 使用 `std::move_only_function`；
- C++20 使用兼容实现；
- 支持 noexcept/cv/ref qualified signature；
- SBO 大小作为 implementation detail 或显式 ABI policy；
- 空调用通过 contract fail-loud，不依赖 assert；
- allocation failure policy 明确；
- DLL 边界不跨不同 CRT destroy owner。

不要把 `inplace_move_only_function` 合并进同一个类型。它表达“不允许 heap”的更强 contract。

### 5.4 function_ref 与 Delegate

建议同时保留：

| 类型 | 所有权 | 大小 | 可为空 | 用途 |
|---|---|---:|---|---|
| `function_ref<Sig>` | borrowed | 2 words 左右 | 通常否 | 函数参数临时借用 |
| `Delegate<Sig>` | borrowed binding | 2 words | 可选 | 冻结 wiring、member/function bind |
| `move_only_function<Sig>` | owning | SBO/heap | 是 | 跨作用域 owning callback |
| `inplace_function<Sig,N>` | owning fixed storage | 固定 | 是 | no-allocation/hot path |

robotics 当前 two-word Delegate 是合适的上移起点。

### 5.5 FixedText

`FixedText<N>` 适合诊断和无分配状态：

- UTF-8 bytes，不承诺 code-point truncation；
- 截断标志必须保留；
- `assign()` deterministic；
- N 包含 trailing null；
- trivially copyable 与否由实现明确；
- 提供 `view()`/`c_str()`，不隐式转 `std::string`；
- format helper 应返回 truncated 状态。

### 5.6 EnumFlags

提供轻量 trait opt-in：

- 只允许 scoped enum；
- underlying 必须 unsigned 或显式处理符号位；
- `containsAny/containsAll`；
- unknown bits validation；
- 不污染所有 enum 的全局 operator；
- wire decoder 可选择拒绝 unknown bits。

### 5.7 ScopeExit

优先兼容 C++23 `std::scope_exit` 语义：

- move-only；
- `release()`；
- destructor noexcept；
- callable 必须 nothrow invocable，或 failure 进入 contract；
- 可补 `scope_fail/scope_success`，但异常关闭构建下语义要明确。

### 5.8 Contracts

建议 API：

```cpp
LUX_PRECONDITION(expr)
LUX_POSTCONDITION(expr)
LUX_INVARIANT(expr)
LUX_UNREACHABLE(message)
LUX_PANIC(message)
```

`ContractViolation` 至少包含：

- kind；
- expression；
- message；
- source_location；
- thread id 的无分配表示；
- optional build id。

处理规则：

- Release/NDEBUG 不消失；
- 默认 handler emergency write 后 abort；
- 不走常规日志队列；
- 不分配；
- 不抛异常；
- handler 只允许启动期安装；
- 测试通过子进程 death contract，不在同进程 longjmp 恢复。

### 5.9 Checked arithmetic

推荐：

```cpp
checked_add(a, b)
checked_sub(a, b)
checked_mul(a, b)
checked_narrow<T>(value)
saturating_add(a, b)
align_up_checked(value, alignment)
```

结果返回 expected/optional，不通过 UB、异常或 silent wrap 表达失败。

这是 wire size、队列 byte budget、GPU allocation、shared-memory framing 的共同基础。

---

## 6. 时间与单位

### 6.1 不重造 std::chrono

`Duration` 继续使用 `std::chrono::duration`。lux-cxx 解决的是 clock domain 混用，而不是
替代 chrono。

推荐：

```cpp
template <class ClockDomain, class Duration = std::chrono::nanoseconds>
class Timestamp;

struct SteadyClockDomain;
struct SystemClockDomain;
template <class Tag> struct SensorClockDomain;
template <class Tag> struct RemoteClockDomain;
```

不同 domain 的 Timestamp 不可直接相减或比较。

### 6.2 ClockMapping

robotics/communication 需要 sensor/remote→steady 的映射：

```cpp
template <class From, class To>
struct ClockMapping
{
    double scale;
    Duration offset;
    Duration uncertainty;
    Sequence revision;
};
```

映射必须说明：

- 来源观测区间；
- 是否允许 extrapolation；
- uncertainty；
- revision；
- overflow/saturation；
- clock reset/jump 处理。

### 6.3 ManualClock

ManualClock 用于：

- timer/timeout 单元测试；
- queue blocking adapter；
- retry/backoff；
- robotics sensor playback；
- engine close watchdog contract。

不要把 deterministic executor 与 ManualClock 强耦合；二者可以组合但独立测试。

### 6.4 物理单位

`Bytes`、`Frequency`、`Angle` 有跨项目价值，但建议是独立轻量 `units` component：

- core 不默认 include；
- 底层 Rep 与 ratio 明确；
- wire 单位显式；
- 避免与 Eigen/GLM 类型系统绑定；
- 先从 Bytes/Frequency/Angle 三个高频量开始，不一次实现完整 SI 库。

---

## 7. Bytes、Digest 与 Binary Serialization

### 7.1 SharedBytes

lux-engine 当前 `SharedBytes` 把 owner 与 span 绑定，是合适的抽取对象。

推荐类型族：

```text
ByteView            borrowed immutable span
MutableByteView     borrowed mutable span
SharedBytes         shared owning immutable range
UniqueBytes         unique owning mutable buffer（仅有真实需求时）
```

SharedBytes contract：

- 非空 view 必须有 owner；
- owner 与 range 在同一构造操作中绑定；
- subspan 继续共享 owner；
- 临时 vector/string 不可隐式转 ByteView；
- 跨线程复制安全只保证 owner lifetime，不保证 owner 指向对象可变状态线程安全；
- `copyOf` 是显式分配 API；
- 空 bytes 的 owner 语义固定。

### 7.2 Digest

合并顺序：

1. 以 lux-cxx SHA-256 test vectors 为算法 SSOT；
2. 建立 `Digest256` value；
3. 补 hex parse/format；
4. engine `ContentDigest256` 迁移；
5. robotics `Sha256Digest` 迁移；
6. 删除 robotics 局部 SHA 和 engine 重复 hasher。

测试：

- NIST/公开向量；
- incremental chunk boundaries；
- empty/large input；
- constexpr 与 runtime 一致；
- hex uppercase/lowercase policy；
- invalid parse；
- cross-platform exact bytes。

### 7.3 Binary foundation

建议建立不依赖 reflection 的 component：

```text
lux/cxx/binary/
  cursor.hpp
  reader.hpp
  writer.hpp
  endian.hpp
  varint.hpp
  limits.hpp
  error.hpp
  canonical.hpp
```

Reader/Writer 必须支持：

- fixed little/big endian；
- exact-width integer；
- varint/zigzag；
- bounded strings/arrays；
- checked offset/size；
- max bytes/count/depth；
- exact consume/no trailing；
- structured error `{code, offset, context}`；
- no exception；
- allocation-free primitive read；
- caller-owned allocation policy。

### 7.4 Canonical encoding

确定性规则必须写进 contract：

- map/key sort；
- duplicate key rejection；
- normalized boolean；
- floating NaN canonicalization 或拒绝；
- negative zero policy；
- UTF-8 validation/normalization policy；
- reserved bits/fields；
- trailing bytes；
- padding 必须为零；
- re-encode exactness。

### 7.5 Reflection adapter

Reflection serialization 是 binary foundation 的上层 adapter：

- binary core 不 include reflection；
- reflection adapter 把 metadata field 映射到 binary field；
- AssetRef、EntityScene relocation 等 domain policy 留在 consumer；
- unknown fields 与 schema migration 由上层决定；
- generated metadata IR 不绑定具体 archive backend。

### 7.6 Fuzz 与 golden vectors

每个 wire component 至少有：

- empty/truncated/trailing；
- malicious count/length；
- recursion/depth；
- integer overflow；
- duplicate/unsorted；
- invalid enum/flags；
- deterministic re-encode；
- version boundary；
- corpus regression；
- libFuzzer/AFL-compatible entry；
- Windows/Linux/Android golden bytes。

---

## 8. Memory 与 Container

### 8.1 PMR resources

建议增加：

- `CountingMemoryResource`；
- `BudgetMemoryResource`；
- `FailingMemoryResource`；
- `FixedBlockMemoryResource`；
- `MonotonicScratchResource`；
- `NoGrowScope`。

每个类型必须明确：

- upstream owner；
- thread safety；
- reset/close；
- outstanding allocation；
- alignment；
- zero-size allocation；
- telemetry 是否 atomic；
- failure 是 nullptr、expected 还是 process contract。

不建议在通用 API 中使用 `FrameArena` 名字；engine 可以 typedef。

### 8.2 SlotMap

现有 dense SlotMap 的改进项：

- allocator-aware；
- non-throwing try_emplace；
- reserve failure contract；
- deterministic iteration 文档；
- move/copy policy；
- generation exhaustion test；
- high-index/recycle stress；
- exception/no-exception 两种构建；
- pointer invalidation 更醒目。

### 8.3 StableSlotMap

以 robotics 实现为起点，要求：

- 地址稳定；
- generation key；
- optional per-record auxiliary state；
- erase/reuse 不 ABA；
- allocator-aware node storage；
- deterministic logical iteration；
- clear 后 key 失效；
- 不因 vector relocation 改变 record 地址。

### 8.4 SmallVector

当前实现已存在，不应重写。先做：

- standard container conformance tests；
- non-trivial/move-only type；
- over-aligned type；
- SBO→heap→SBO 是否支持及语义；
- exception/no-exception；
- iterator invalidation；
- sanitizer；
- benchmark 证明 allocator-aware 的必要性。

只有真实 consumer 需要 PMR/allocator 时再扩 API。

### 8.5 ObjectPool

重点审计：

- destructor 是否覆盖所有 live object；
- stale handle/pointer；
- alignment；
- double free；
- pool move；
- thread ownership；
- trim/reclaim；
- upstream failure；
- sanitizer churn。

不要让 ObjectPool 演化成通用 ECS storage。

---

## 9. 并发队列与通道协议

### 9.1 不建立万能 Channel

SPSC、bounded MPMC、blocking queue、shared-memory ring 的内存模型和使用约束不同，
不应强行统一实现。

应该统一的是状态词汇和 conformance：

```text
Admission: OPEN -> CLOSING -> CLOSED
Push:      ACCEPTED | FULL | CLOSED | TOO_LARGE
Pop:       VALUE | EMPTY | CLOSED_AND_DRAINED
Close:     stop accepting, optionally drain accepted values
```

### 9.2 Endpoint

推荐 producer/consumer 分离：

```cpp
Producer<T>
Consumer<T>
ChannelOwner<T>
```

这样：

- producer 不能 pop/close owner；
- consumer 不能伪造 producer；
- owner 生命周期明确；
- cached endpoint 使用 generation-safe control；
- close 与 producer count 可线性化。

### 9.3 Capacity 与 byte budget

element count 不足以约束动态 payload。通用 admission 可以支持：

- item capacity；
- byte budget；
- per-item maximum；
- reservation lease；
- release on terminal disposition；
- high-water telemetry。

具体 payload 成本估算由 consumer 提供，lux-cxx 不认识 decoded/GPU/Scene 工作集。

### 9.4 Close gate

跨项目重复出现的模式是：

- admission state 与 producer count 在同一原子 word；
- producer CAS 成功后持 ticket；
- close 阻止新 producer；
- 最后一张 ticket 释放时唤醒 owner；
- ticket 保存 generation-safe control；
- owner 销毁前必须证明 tickets 为零。

这可以成为一个小型 `AdmissionGate`，但不包含 scheduler、thread pool 或 callback graph。

### 9.5 Blocking adapter

BlockingQueue 应是非阻塞 core queue 的可选 adapter，或至少共享状态结果：

- 接 `stop_token`；
- timeout 使用 injected/manual clock 或标准 steady clock adapter；
- close 唤醒全部 waiter；
- drain accepted values 的规则一致；
- destructor 要求 producer/consumer 已停止，不能隐式掩盖并发 UAF。

### 9.6 Memory model 和测试

每个 lock-free 类型必须文档化：

- producer/consumer 数量；
- happens-before；
- false-sharing alignment；
- object construction/destruction；
- close 与 in-flight push 竞态；
- approximate size 的含义；
- TSAN 预期；
- unsupported T traits。

测试包括 wrap-around、capacity 1/2、close race、move-only payload、destructor accounting、
16+ producer stress（MPMC）、长时间 randomized schedule。

---

## 10. Diagnostics、ABI 与 Reflection

### 10.1 DiagnosticRecord

lux-cxx 只提供 value，不决定输出位置：

```cpp
struct DiagnosticRecord
{
    Severity severity;
    StableNameIdView<CategoryTag> category;
    StrongId<DiagnosticCodeTag, std::uint32_t> code;
    Timestamp<SteadyClockDomain> timestamp;
    FixedText<...> message;
    std::source_location source;
    TraceId trace;
    SpanId span;
};
```

约束：

- record 可复制/移动；
- hot/failure path 有固定上限；
- sink 由 engine/robotics/communication 宿主装配；
- OpenTelemetry、stderr、logcat、文件、UI 都是 adapter；
- 不提供 global logger singleton。

### 10.2 Dynamic library 与 ABI

OS loader 更适合 `lux-platform`。lux-cxx 可以提供 ABI value/primitives：

- SemanticVersion；
- BuildId；
- CompilerAbi；
- RuntimeLibraryAbi；
- AbiFingerprint；
- C-compatible StringView/ByteView；
- versioned descriptor header。

禁止把 engine 的 Extension descriptor/registrar 整体搬进 lux-cxx。

跨 DLL/so seam 优先：

```text
POD descriptor
struct_size + abi_version
const char* + size
const byte* + size
explicit create/destroy/shutdown
host allocator callback（若需要）
```

避免直接穿越 `std::string/vector/shared_ptr/function`，除非 ABI 明确锁定同 compiler/CRT
并由完整 fixture 验证。

### 10.3 ABI fingerprint

至少覆盖：

- compiler vendor/version；
- target triple/pointer width；
- endianness；
- CRT/STL vendor/version/mode；
- MSVC MD/MDd/MT；
- `_ITERATOR_DEBUG_LEVEL`；
- libstdc++ C++11 ABI/debug mode；
- RTTI；
- exceptions；
- sanitizer/instrumentation（若影响 ABI）；
- build/API major。

### 10.4 Reflection metadata IR

目标是 frontend-independent metadata：

```text
Clang parser ------\
                    -> stable metadata IR -> code generators/runtime adapters
C++ static reflect-/
```

IR 应表达：

- canonical type/schema name；
- fields/methods/enums；
- source location；
- attributes key/value；
- type relationships；
- version；
- deterministic ordering。

IR 不应内置：

- ECS component；
- EntityScene persistence；
- Render pass/operation；
- Editor inspector policy。

这些通过独立 annotation family 和 consumer adapter 解释。

### 10.5 C++26 reflection

未来静态反射只替换 metadata frontend：

- 不改 wire schema identity；
- 不改 serialization framing；
- 不改 ECS/Render annotation policy；
- Clang 与 static reflection 生成同一 IR golden data；
- 两 backend 可并存一段时间。

---

## 11. 测试、CI、发布与 API 分级

### 11.1 当前缺口

当前 `.github/workflows/deb.yml` 主要覆盖 Ubuntu amd64/arm64 Release package：

- reflection test 关闭；
- serialization test 关闭；
- concurrent/memory/compile_time/event 默认关闭；
- 缺 MSVC/Clang-cl；
- 缺 Debug/RelWithDebInfo；
- 缺 sanitizer；
- 缺 installed consumer；
- 缺 DLL ABI fixture。

基础库在扩大 API 前必须先补这些门禁。

### 11.2 CI 矩阵

| 维度 | 必选 |
|---|---|
| OS | Windows、Ubuntu；Android arm64 compile/install |
| 编译器 | MSVC、Clang-cl、GCC、Clang |
| 标准 | C++20、C++23 |
| 配置 | Debug、RelWithDebInfo |
| ABI mode | RTTI/exceptions on/off；CRT/STL debug mode |
| sanitizer | ASan、UBSan、TSan（支持平台） |
| package | build-tree consumer、install-tree consumer |

PR 快速矩阵可以裁剪，nightly/release 必须完整。

### 11.3 每个 public header 独立编译

自动生成一个 TU/头：

```cpp
#include <lux/cxx/.../Header.hpp>
int main() {}
```

分别在 C++20/C++23、exceptions/RTTI on/off 编译，防止隐式 include 和宏污染。

### 11.4 Installed consumer

测试流程：

1. 安装 lux-cxx 到空 prefix；
2. 启动独立最小 CMake 项目；
3. `find_package(lux-cxx CONFIG REQUIRED COMPONENTS ...)`；
4. 只使用安装头/lib/config；
5. build/run；
6. 检查 available-components、transitive dependency 和版本。

不能从 source/build tree 偷 include。

### 11.5 DLL/DSO boundary

真实 fixture 覆盖：

- callback create/destroy 跨边界；
- allocator ownership；
- thread_local 唯一性；
- RTTI/exceptions mode mismatch 拒绝；
- CRT/STL mismatch tag；
- descriptor size/version；
- delayed callback after unload fail-loud；
- Windows DLL 与 Linux so。

### 11.6 Sanitizer 与压力测试

- SlotMap/StableSlotMap/ObjectPool randomized churn；
- queue wrap/close/race；
- serialization fuzz corpus；
- allocation failure injection；
- alignment/over-aligned types；
- move-only/non-trivial destructor；
- TSan producer/consumer；
- ASan stale key/owner misuse fixture。

### 11.7 API 分级

每个 public API 标记：

- Stable：semver 约束；
- Experimental：可在 minor 变更，但有 changelog；
- Internal：不安装；
- TestSupport：只供测试 package。

不要通过目录猜 API 稳定性。

### 11.8 版本与兼容策略

- package 使用 semver；
- header-only template 变化也算 API/ABI 风险；
- wire schema version 与 package version 分离；
- deprecated API 必须有 removal version；
- major cut 不保留无限期 shim；
- ABI report 和 API diff 进入 release artifact。

---

## 12. 推荐 component 结构

```text
lux-cxx/
  core/                  result, callable, IDs, contracts, checked arithmetic
  bytes/                 byte views and owned bytes
  digest/                digest values + algorithms
  time/                  clock-domain timestamp/manual clock
  units/                 optional Bytes/Frequency/Angle
  container/             SlotMap/StableSlotMap/SmallVector/...
  memory/                PMR resources/ObjectPool/intrusive_ptr
  concurrent/            SPSC/MPMC primitives and admission endpoints
  binary/                bounded deterministic binary encoding
  reflection/
    ir/
    runtime/
    clang_frontend/
  serialization/         higher-level reflection adapters/json/xml
  diagnostic/            DiagnosticRecord only
  abi/                   ABI value/fingerprint/POD views
  test_support/          manual clock, allocation gate, failure injection
```

不是所有目录都要一次建立。建议第一轮只创建：

```text
core
bytes
digest
time
binary
test_support
```

其他目录在迁移现有 component 时演进。

---

## 13. 跨项目迁移清单

### 13.1 lux-engine

第一批：

- `SharedBytes` → lux-cxx bytes；
- `ContentDigest256/Sha256Hasher` → lux-cxx digest；
- `BasicOwnedStableId` generic 部分 → StableNameId；
- 两份 `BasicUuid` → UuidId；
- 局部 ScopeGuard/EnumFlags → core；
- wire codec 的 checked arithmetic → binary/core。

保留在 engine：

- ExtensionId/ContributionId tag 和 canonical reverse-domain policy；
- EntityScene schema/content domain separation；
- AssetRef/relocation；
- Registry/Schedule/AsyncRuntime/Render protocol。

### 13.2 lux-robotics

第一批：

- runtime `StableId` → StableNameId/View；
- runtime `Delegate` → core Delegate；
- runtime `FixedText` → core FixedText；
- runtime `Sha256Digest` 和局部 SHA → digest；
- map `StableSlotMap` → container StableSlotMap；
- timestamp_ns 逐步加 clock-domain wrapper。

保留在 robotics：

- SLAM/odometry contract IDs 的 Tag；
- sensor/pose/map 领域类型；
- Eigen/李群数学；
- pipeline/executor 业务调度。

### 13.3 lux-communication

第一批：

- TimestampS adapter 到 clock-domain time；
- shared-memory framing 使用 binary checked cursor；
- ring push/close status 与 concurrent conformance 对齐；
- FragmentAssembler checked size/budget；
- ABI-safe byte/string views。

保留在 communication：

- topic/service discovery；
- TCP/SHM transport；
- publisher/subscriber QoS；
- message schema/codegen；
- network retry/heartbeat policy。

### 13.4 迁移原则

- 先在 lux-cxx 建 API + test；
- 再迁一个真实 consumer 验证；
- 再迁第二个 consumer，证明抽象成立；
- 最后删除重复实现；
- 不先删旧实现再边编译边猜 API；
- 不保留无期限 alias/shim；
- 每批迁移都有 exact ownership/wire/ABI tests。

---

## 14. 分阶段执行计划

### Phase 0：基线与门禁

- 记录当前 API/component/package 清单；
- 打开现有全部测试；
- 建 Windows/Linux C++20/C++23 matrix；
- 建 installed consumer；
- 建 public-header self compile；
- 标记 Stable/Experimental/Internal。

完成标准：不新增 API 也能获得可信红绿结果。

### Phase 1：Identity 与 Core

- canonical core component；
- expected facade 迁移；
- callable family；
- contracts；
- StrongId/StableNameId/UuidId/SchemaVersion；
- TypeToken 防持久化门禁；
- LFGR v3 consumer migration。

完成标准：新 wire 中 compiler type identity 为零。

### Phase 2：Bytes、Digest、Binary

- SharedBytes；
- Digest256/SHA；
- checked cursor/reader/writer；
- canonical encoding；
- engine/robotics 重复 digest 删除；
- fuzz/golden vectors。

完成标准：三个项目使用同一 digest value 和 binary limits 语义。

### Phase 3：Time、Container、Memory

- Timestamp/ClockMapping/ManualClock；
- StableSlotMap；
- SlotMap/SmallVector/ObjectPool hardening；
- PMR budget/failure resources；
- sanitizer/churn。

完成标准：robotics map 不再维护私有 StableSlotMap，时间域不可误传。

### Phase 4：Concurrent protocol

- queue result vocabulary；
- endpoint；
- admission gate；
- byte budget reservation；
- stop-token blocking adapter；
- SPSC/MPMC/shared-memory conformance。

完成标准：共享状态语义统一，但 engine/communication 各自 runtime 保持独立。

### Phase 5：Diagnostics、ABI、Reflection

- DiagnosticRecord；
- ABI fingerprint/POD view；
- reflection metadata IR；
- Clang frontend adapter；
- static-reflection feasibility；
- release ABI/API report。

完成标准：generic reflection IR 不出现 ECS/Render/Scene 词汇。

---

## 15. 验收门禁

### 15.1 Architecture

- lux-cxx public headers 不 include lux-engine/robotics/communication；
- 不出现 Entity/Scene/Asset/Render/SLAM/Topic 领域类型；
- 不新增全局 singleton logger/event bus/runtime；
- 每个新 primitive 至少两个真实 consumer 或标 Experimental。

### 15.2 Identity

- serializer/wire writer 中 `type_hash/type_name` 为零；
- stable-name equality 比较完整名字；
- UUID/digest/slot/strong id 不可隐式互转；
- hash/digest 有固定 golden vectors。

### 15.3 Ownership

- SharedBytes 非空 owner invariant；
- callable create/destroy owner 一致；
- queue endpoint 不裸借 owner；
- admission ticket close 后结清；
- container stale key fail-safe。

### 15.4 Quality

- MSVC/GCC/Clang；
- C++20/C++23；
- Debug/RelWithDebInfo；
- sanitizer；
- installed consumer；
- header self compile；
- DLL/DSO fixtures；
- fuzz/golden vector；
- second incremental build no work。

### 15.5 Release

- API diff；
- ABI fingerprint/report；
- component list；
- migration guide；
- deprecated removal schedule；
- package provenance/build id；
- cross-project compatibility matrix。

---

## 16. 风险与需要避免的误区

### 16.1 一次性大迁移

同时改 namespace、package、类型和 wire 会让失败无法定位。每批只改变一个主边界，并保留
golden data 与 installed consumer。

### 16.2 把重复代码误判为共同抽象

两个项目都有 `Runtime`、`Handle` 或 `Queue` 不代表语义相同。必须比较 ownership、close、
thread、failure、wire lifetime。

### 16.3 策略模板爆炸

SlotMap/StableSlotMap、SPSC/MPMC、function_ref/Delegate 应保持不同具体类型，共享小原语和
conformance tests，不做万能模板。

### 16.4 标准库门面漂移

fallback 必须与 std 版本有同一测试。不能 C++20 和 C++23 下行为、异常或 ABI 悄悄不同。

### 16.5 只构建 lux-engine

lux-cxx 的改动必须由 robotics、communication 的独立 consumer 验证，不能以 engine 全量
构建通过代替跨项目兼容证明。

### 16.6 shim 永久化

canonical include 迁移可以有短暂 deprecated forwarding，但必须写 removal version；项目簇
能同步升级时优先 hard cut。

---

## 17. 最终优先级 Backlog

### P0

1. CI/测试基线；
2. core component；
3. TypeToken/StableNameId/UuidId/SchemaVersion；
4. contracts；
5. LFGR v3 与禁止持久化 type_hash 门禁。

### P1

1. SharedBytes；
2. Digest256/SHA 收敛；
3. binary foundation；
4. FixedText/Delegate/ScopeExit/EnumFlags；
5. StableSlotMap；
6. Timestamp/ManualClock；
7. checked arithmetic/PMR budgets。

### P2

1. queue endpoint/admission conformance；
2. DiagnosticRecord；
3. ABI-safe views/fingerprint；
4. reflection metadata IR；
5. API/ABI compatibility automation；
6. C++ static-reflection backend feasibility。

### 明确拒绝

1. 新 ECS；
2. 新事件总线；
3. 新万能 async runtime/thread pool；
4. engine/robotics 数学统一；
5. Asset/Scene/Render/SLAM/Communication 领域 API；
6. compiler type hash 持久化。

---

## 18. 最终建议

如果近期只能完成三项，应按以下顺序：

1. **Identity + LFGR v3**：先消除真实的跨编译器数据兼容风险；
2. **Core + quality matrix**：统一 result/callable/contracts/ID，并让所有配置真正受测；
3. **Bytes + digest + binary foundation**：为 engine、robotics、communication 提供共同 wire
   和所有权底座。

这三项完成后，再推进时间、StableSlotMap、通道和 reflection。这样 lux-cxx 会逐步变成
项目簇可信赖的基础，而不会膨胀成新的领域框架。
