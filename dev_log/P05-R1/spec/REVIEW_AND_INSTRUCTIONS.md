# P05 复审 R1：保存回调生命周期与回执清理后的版本衔接

**日期：2026-09-29**  
**项目：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**审阅验收提交：`5866f990f8a5c4d68e19e0e395c9487c6c0f2779`**  
**P05 实现提交：`8deaee9beaab431c1cfa82282206e7ff2f40d09b`**  
**前置：P04 R1，`aad13c594afe33af69fcfba1b44790b4e9ad148d`**

## 0. 结论与执行边界

**P05 主体成果认可；暂不进入 P06。本轮只补正两个问题组，补正后停在 P05 复审。**

- **B01：SaveService 的角色回调与记录生命周期没有完全闭合。** describe 回调撤销自身注册后仍继续调用 capture；accept 回调递归推进并确认同一操作后，外层仍写已释放记录。
- **B02：同源写入衔接依赖可被 acknowledge 删除的观察记录。** 一次合法的旧结果确认，会改变后续同源保存是否被误报 CONFLICT。

这不是要求重做三类 Session、History、SessionState、Codec、WriteCoordinator 或执行器。不是新增插件系统、通用 OperationManager 或另一套全局 busy。目录、target、SDK 包名保持原有组织。本轮不实施 P06，也不把新缺陷挂到旧 C01/C03/C04。

原 `dev_log/P05/` 的 104/104 是既有测试的历史成绩；发现新负例不应改写其日志或收据。新增记录冻结到 `dev_log/P05-R1/`，当前施工材料继续只维护 `.internal/editor-redesign/`。

## 1. 审阅与独立验证的实际范围

### 1.1 已读取的内容

读取了当前分支与提交父链、P05 README/文件清单、SaveService、WriteCoordinator、编码拥有者、三模型测试、Scene/Material 保存角色、Scene 持久化访问与 codec、真实文件发布和执行器绑定，以及 ExecutionRuntime 的结果交付实现。另读取了 P05 的 CTest 归档和原 P05 实施契约。

本轮没有逐行审完所有新增文件、逐份复算所有远端归档哈希，也没有独立执行完整 Lux 构建、104 项 CTest、九组安装消费者、实际文件 IO、GPU、Android 或真实 DLL 卸载测试。

### 1.2 核心源码隔离探针——不是完整 SDK 验收

为了不只依赖静态推导，`probes/` 实际编译了两份与 GitHub 返回 Blob 一致的生产 `.cpp`：

| 文件 | Git Blob SHA-1 |
|---|---|
| `SaveService.cpp` | `78784eb7047bf7c1b939a83c5c771e2033196bac` |
| `WriteCoordinator.cpp` | `3f8fa33d91f671a2de1c8f494148d36501ea1fb9` |

文件通过连接器读取后保存，按 `SHA1("blob " + byte_length + NUL + bytes)` 核对。**这两份实现没有为触发问题而修改。**

但本环境没有完整 Lux 依赖：

- 使用明确标记的测试头 shim，提供接口和值的形状。
- `lux::cxx::expected` 以 `std::expected` 代替；基础身份和 CodeLease 也有简化替身。
- 编译使用 GCC 14.2，C++23 仅为了这个 expected 替身；**不要求项目从 C++20 升级**。
- 使用内存存储与受控 ISaveSource，不是真实 Scene/Material/Flow 或 ProjectArtifactStore。
- O0、O2 均启用 AddressSanitizer/UndefinedBehaviorSanitizer。

因此，下文称为“核心源码隔离复现”，不能写成“已在完整引擎/真实模型/Windows 上复现”。完整 SDK 与真实模型的新回归仍由实施方执行。

### 1.3 实际结果

| 场景 | O0 | O2 | 目标契约 |
|---|---|---|---|
| 对照：保留 W1 观察记录 | W4 发布，最终 V4，退出 0 | 同左 | 满足 |
| 只增加 `acknowledge(W1)` | W4 误报冲突，最终 V3，退出 10 | 同左 | 不满足 |
| describe 撤销自身注册，角色对象仍活着 | UBSan 空指针引用/调用，退出 1 | 同左 | 不满足 |
| accept 递归采用并 acknowledge 同一操作 | ASan heap-use-after-free，退出 1 | 同左 | 不满足 |

`probes/runs/results.json` 含编译器、命令、源身份、每次返回码和日志。`run_probes.py` 返回 0 仅表示对照和原缺陷观察均按预期出现，**不表示生产实现已经修好**。

## 2. 已成立、不得回滚的设计

以下是本轮应保留的实现方向，不作为新整改任务：

1. SaveService 不拥有 Session；SessionStore、SessionState 和 History 仍维护各自唯一事实。
2. 写票据在捕获前预留；编码完成顺序与同目标发布顺序分开。
3. PublicationUnknown 保留目标责任和编码产物，writer 未退休时不放走后继。
4. 普通保存冻结后允许编辑；后台收到拥有输入，而不是 live Session/ReadView。
5. Save As 使用 BindingChangePermit 和预备 rebind，不通过 Reload 或清空 History 实现。
6. Export Copy 不采用当前保存基线；迟到正常完成会保留磁盘事实并拒绝失效 Session。
7. ProjectArtifactStore 复用文件发布算法，命名发布与进一步持久性确认分开。
8. 文档明确只有接入新协调器的生产者才受排序保护，旧 writer 没有被冒称已迁移。

这些不等于全路径已证明。特别是“代码保活”不等于“角色仍注册”，“记录地址稳定”不等于“记录不能被递归删除”。

## 3. B01：角色回调生命周期与操作执行期没有闭合

### 3.1 精确位置

```text
editor/persistence/src/SaveService.cpp
    SaveSourceRegistration::~SaveSourceRegistration()
    SaveService::requestSave()
    SaveService::adoptCompletions()
    SaveService::acknowledge()

editor/persistence/include/lux/engine/editor/persistence/SaveSource.hpp
    ISaveSource
    SaveSourceRegistration

editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp
    owner 线程与完成推进契约
```

当前公开说明要求 owner 线程和先撤销注册再销毁角色；没有在类型或入口检查中保证“回调执行期间不重入”。本问题在同一线程中发生，不需要数据竞争，也不需要删除正在执行的角色对象。

### 3.2 B01-a：describe 返回后，注册可能已经被撤销

`requestSave()` 的关键顺序（审阅版本源码行 174、204）：

```cpp
auto info = describeSource(*registration->source);
// 校验 info，预留 ticket/operation/allowance……
auto frozen = captureSource(*registration->source, *info, request, allowance);
```

`registration` 是 shared_ptr，保证 State 对象存在；但注册 token 析构会把其中的 `source` 置空。

实际隔离探针的受控回调：

1. 正常注册 Source，保存 token。
2. 此后 Source::describe() 在返回合法描述前销毁自己的 token。
3. Source 对象本身仍然活着，不做自删除、不释放 DLL。
4. `requestSave()` 没有在回调后重新验证 source，继续解引用空指针。

观察到：

```text
registration_revoked_inside_describe=1 source_still_alive=1
SaveService.cpp:204: runtime error: reference binding to null pointer of type 'ISaveSource'
```

**需要满足的契约：** 撤销只切断未来可调用性，不能成为空指针调用；本次请求应明确失败，不能改用同 Session 后来注册的另一角色继续原请求。尚未产生的物理副作用不能凭空登记为 Published；已预留资源必须可回收。

### 3.3 B01-b：递归采用与确认可释放外层仍在使用的操作

当前 `adoptCompletions()` 保存 `Operation& op`，然后调用：

```cpp
op.outcome->adoption = op.registration->source->accept(std::move(saved));
```

代码用 `unique_ptr<Operation>` 和 ID 快照避免 vector 扩容使引用失效，这是有效但不完整的保护。

受控 accept 回调可以：

1. 再调用一次同一 SaveService::adoptCompletions()。
2. 嵌套调用再次处理同一 AWAITING_ADOPTION 记录；其 accept 这次直接返回。
3. 嵌套调用把记录设为 TERMINAL。
4. 回到外层 accept 的函数体，调用 `acknowledge(id)`；此时记录允许删除。
5. 外层 accept 返回；第一层 `op.outcome->adoption = ...` 写入已释放内存。

隔离探针输出：

```text
nested_accept_calls=2 acknowledged_inside_accept=1
AddressSanitizer: heap-use-after-free
WRITE ... SaveService::adoptCompletions() ... SaveService.cpp:308
```

角色一直活着；删除的是正在被第一层采用流程借用的保存记录。因此单纯 pin 注册 State 或将 operations 换成 deque/list 都不能修复。

### 3.4 B01 的修正约束

先明确本服务自己的回调调度契约，再在入口实施。建议采取最小的 owner 线程 RAII 调度保护，不新增线程锁或通用任务管理器。

必须同时满足：

- 调用 extensible describe/capture 后重新验证原注册仍可调用；code lease 和 State 存活不能代替这一步。
- 原角色失效时停止本次角色调用，不把同 Session 新注册项接到旧请求上。
- 同一个操作的 accept/rebind 不能递归执行。嵌套 `adoptCompletions()` 可以明确延迟或不重复进入，随后外层/下一 owner 安全点继续处理。
- 正在执行 accept/rebind 及其清理的操作不能被 acknowledge 销毁。`AWAITING_ADOPTION`（业务等待）与“正在执行角色调用”不是同一事实；内部保护要能区分。
- 回调之后要么持有保证操作尚在的调用作用域，要么按 ID 重新查找；不能继续使用允许被删的裸引用。
- 只读 status 可保持可用。对会改变当前调度的不允许重入操作，应返回已有 BUSY/NOT_TERMINAL 等准确结果，或明确定义延迟规则；不要终止进程、静默丢请求。
- 注册 token 的 RAII 撤销仍应有效，不能为了防止空指针而禁止析构或永久保活全部角色。
- 角色回调若能够重入 requestSave/registerSource，需要保护准入计数；不能在回调前检查一次容量，回调后继续无条件提交超额操作。
- 任何已产生的 CommitReceipt 都保持为磁盘事实；角色失效或被延迟不会把它改成 NotPublished。

可以使用服务私有 dispatch scope 或每操作 in-flight scope。它维护的是 **SaveService 自己的记录执行期**，不是另一份 Session 的编辑 busy，不能与 SessionState gate 双写。具体字段数量由实际方案决定，不为文档新造一组永久“管理器”。

不要求支持任意对象自删除或任意非法跨线程调用；本轮负例不需要这些行为。若某种重入明确禁止，必须可检查地拒绝，不能靠注释容忍内存错误。

## 4. B02：确认历史观察记录改变了合法同源保存的结果

### 4.1 精确位置与责任混淆

```text
editor/persistence/src/WriteCoordinator.cpp
    Impl::Lane
    Impl::settle()
    WriteCoordinator::takeReady()
    WriteCoordinator::acknowledge()
```

`takeReady()` 使用 lane.chain_base、lane.version，或仍在 records 中的历史 CommitReceipt.version，判断新票据的 expected_version 是否属于相同写入链。

而 `acknowledge()` 会删除该历史 Record。只要同 lane 还有其他记录，Lane 仍在，但用于证明某个中间版本的旧 receipt 可能已经消失。

这把两种责任耦合了：

- 可被用户确认清除的历史观察记录；
- 后续合法请求仍需使用的同源版本连续性。

### 4.2 使用 SaveService 公共 API 的准确失败顺序

同一 Session、同一 BindingRevision、同一规范化目标；没有外部写者，也没有换 Session。

| 步骤 | 磁盘 | 保存角色已采用的目标版本 | 操作 |
|---|---|---|---|
| 0 | V0 | V0 | 初始状态 |
| 1 | V1 | V1 | W1 保存、发布、采用；不确认删除 W1 |
| 2 | V1 | V1 | 准入 W2、W3；二者都基于已知 V1 |
| 3 | V3 | V1 | W2、W3 按顺序编码/发布完成；尚未调用 adoptCompletions |
| 4 | V3 | V1 | 只 acknowledge 已经 TERMINAL 的 W1 |
| 5 | V3 | V1 | 同角色请求 W4，捕获更新内容，目标前提仍是 V1 |
| 6 | V3 | — | 当前代码误报 CONFLICT，W4 未发布 |

物理完成和基线采用是公开设计中明确分离的步骤，所以步骤 3 并不违反顺序契约。步骤 4 只确认早已采用的 W1，没有确认未采用的 W2/W3，也没有直接修改 Coordinator 私有状态。

如果唯一的区别是**省略步骤 4**，W1 的 receipt 仍在，W4 就被识别为链上后继，顺利发布成 V4。

隔离核心的实际输出：

```text
对照：ack_first=0 fourth_published=1 final=V4 conflicts=0
缺陷：CONFLICT expected=V1 actual=V3
      ack_first=1 fourth_published=0 final=V3 conflicts=1
```

它不造成这次探针中的文件损坏，而是合法最新保存被拒绝。不能因为它表现为错误返回而忽略；观察记录确认时机不应决定是否允许同源保存。

### 4.3 B02 的修正约束

不指定一条未经完整论证的替换表达式。要求在现有 Coordinator/Record/Lane 内建立**有界、与观察记录清除解耦的连续性证据**。

可考虑明确区分请求原始观察前提、协调器已验证的有效发布前提、同源链边界及仍未完成/采用请求所需锚点。若需要增加字段，应说明它是不可变请求事实还是当前协调状态，不再用一个 expected_version 字段承载多个阶段含义。

一个应优先检查的最小修正点：本次反例中，尚未确认的 W2 已经以 V1 为有效前提成功发布，这个“成功提交消费了 V1”的事实仍在记录里；当前 `has_chain_version` 只查提交产物版本而不利用该已验证前提。可以在严格的同目标、同 origin、当前链边界和先后票据条件下，论证是否利用现存已提交记录的有效前提即可完成有界衔接。不能把尚未成功发布的任意请求声称的 expected_version 当成已经证明的版本关系。若采用这一窄方案，也必须通过下面的链打断、确认次序和外部冲突测试，而不是只加一个不带条件的 OR。

必须做到：

- W1 已确认后，仍能证明 W4 的 V1 属于当前同 Session/Binding 的合法前序链，并使 W4 在真实当前受控前提上发布。
- 新 Session、不同 binding、别的工作副本、匿名生产者，不能借用前一会话的衔接资格。
- 外部修改后端当前版本，仍应触发真实冲突；不能通过“发布前先 resolve 当前文件再无条件更新预期”抹掉并发修改。
- 中间被另一个工作副本合法写入后，原来源不能无条件延续自己的旧链。
- 继续按 ticket 顺序发布，不按 StateId 的数值大小排序；失败空洞、取消、Unknown 的隔离不回退。
- 终态观察记录仍可确认并回收；不要为证明版本谱系保留无限的旧 receipt/版本列表。
- 不要求普通保存先同步等待所有旧回执采用；读取 gate 可能暂时阻止采用，协调器本来也允许多个新生产者使用。
- 不要让 acknowledge 悄悄返回成功却永久不删除数据，也不要全局禁用 acknowledge 来掩盖缺口。

**重要：** “origin 相同就无条件把前提改成当前 lane.version”不是充分的修复。还需要验证该请求与当前链的实际连续性，以及链是否已被其他来源/外部修改打断。

## 5. 本轮文件级修改范围

| 文件/符号 | 动作 | 明确不做 |
|---|---|---|
| `editor/persistence/src/SaveService.cpp` | 回调前后注册验证、不可递归处理同操作、执行期防删除及清理；准入检查与回调重入一致 | 不拥有 Session，不把 Published 改成失败，不换成全局事件框架 |
| `.../persistence/SaveService.hpp` | 只有确有需要时补充重入/调度契约；尽量不增加新公共入口 | 不添加泛化工作流 API |
| `.../persistence/SaveSource.hpp` | 明确 token 撤销的可调用性与回调约束 | 不把注册改成永久 Session/adapter 强拥有 |
| `editor/persistence/src/WriteCoordinator.cpp` | 有界同源版本衔接与观察回收分离；保留 FIFO 和 Unknown | 不增加第二协调器、不取消冲突检查 |
| `.../persistence/WriteLane.hpp`、`WriteCoordinator.hpp` | 仅在解决前提语义确需时调整窄值/契约，同步真实调用方 | 不为小值增加库，不改目录/包名 |
| `editor/persistence/test/write_coordinator.cpp` | 同源回执确认时机、不同 origin、链打断、容量/Unknown 回归 | 不降低原断言 |
| `editor/tests/persistence/models.cpp` | 真实三模型延迟采用与 W1 确认；真实角色包装的撤销/重入测试 | 不用本包 shim 冒充真实 SDK |
| 现有 persistence 测试 CMake | 在现有 target 增加场景；文件较长可拆同 target 测试 CPP | 不为每个场景建新业务 target |
| `editor/persistence/README.md` | 写清确认、来源撤销、重入和当前链的契约 | 不把所有限制写成“调用者自觉” |
| `.internal/editor-redesign/` | 更新两问题、测试映射、有限新状态和实际 SHA | 不用原 V4 seed 覆盖当前账本 |
| `dev_log/P05-R1/` | 冻结修复前/后、原回归、源/日志哈希与范围 | 原 P05 及更早验收文件保持历史原样 |

原则上无需改 Scene/Material/Flow 模型、History、SessionStore、SessionState、真实文件发布算法或 ExecutionRuntime。若实际修正必须触及其他生产文件，应在实施记录中说明具体调用依赖，不借机重构全仓。

不新增过渡桥；旧保存 API 和 `SceneSaveCapture::copied` 仍只供原消费者，最迟 P12 删除。不得因本轮新增接口留下第二套同义路径。

## 6. 必测的真实 SDK 场景

这些是原 P05 生命周期、衔接、容量契约的补充组合，不改变 X05-01～09 的含义。建议登记 R05-01～08，沿原 Q11～18/P05 范围标注。

### R05-01：describe 撤销自身注册

用实现 ISaveSource 的测试包装器转发给真实 MaterialSaveSource。注册成功后开启一次性 describe hook：先取得可用描述，然后销毁包装器对应 token，再返回。包装器和底层模型继续存活。

期望：requestSave 明确 STALE_SOURCE 或等价结构化拒绝；capture hook 调用次数为零；无空指针、无泄漏票据/捕获额度；模型、History、binding/checkpoint 不变。重新建立新 token 后正常保存可用。

先在固定修复前版本运行保留结果。不要通过删除 source 对象制造其他不相关 UAF；本场景只撤销注册。

### R05-02：accept 重入采用并确认当前操作

真实文件已经 Published，服务还未完成采用。受控 accept 第一次进入时调用一次 adoptCompletions；随后尝试 acknowledge 同一 SaveId；第二次 hook 不再递归，避免无限递归掩盖问题。

期望：同一操作不被重复 accept；执行期 acknowledge 被拒绝或明确延迟；外层返回后记录安全终结，Committed/Applied 可查询，之后正常 acknowledge 成功。保留真实文件内容和被采用基线。

优先在可用配置用 ASan/UBSan 或同等运行诊断观察；本轮不强制新增全平台矩阵。没有 sanitizer 的平台也必须有次数、返回值和记录生命期断言，不能只跑到进程未崩溃。

### R05-03：回调后失效与准入恢复

在 describe/capture/accept 的允许交接点覆盖：撤销一个角色、新注册同 Session 角色、回调抛普通异常（在非 noexcept 接口上）、尝试嵌套 requestSave、读取 status。

期望：原请求不切换到新角色，不能越过 max_active/terminal/snapshot 容量；不允许的重入被明确拒绝；RAII 状态在成功、拒绝、异常后恢复。Already Published 保留事实，不因采用失败被抹掉。

无需支持任意 adapter 在自身成员函数运行时自删除。需要的测试类型只位于测试文件，不新建全局注册管理器。

### R05-04：W1 已采用并确认，W2/W3 已发布未采用，再请求 W4

使用现有真实模型 Fixture：

1. W1 请求、编码、实际文件发布、服务采用。
2. W2/W3 请求、编码、实际文件依序发布；暂不采用。
3. acknowledge(W1)，确认记录确实消失。
4. 同模型编辑后请求 W4；编码和发布。
5. 文件必须是 W4，不能 CONFLICT；最终采用和 dirty 正确。

至少对 Scene、Material、Flow 各执行一遍；也保留不 acknowledge 的对照。不能改成步骤 2 后先全部采用，因为这会避开原问题。

### R05-05：Coordinator 独立衔接与多种确认时机

在纯 coordinator 测试中覆盖 W1 确认发生于后继 RESERVED、READY、已发布待采用等阶段的组合；后继基于先前已知中间版本。适用时使用连续多个后继和乱序编码。

期望：合法同源最后票据成为最终字节；控制流不依赖保留某个旧观察记录。已确认数据被回收，而不是换成永不清理列表。

### R05-06：延迟采用并非必须同步解决

已有 withRead 正在运行时使基线采用得到 BUSY，随后正常退出；采用等待不会改变物理发布结果。在允许准入的 owner 时点执行后续保存和早期已终态确认。

期望：没有同步等待 owner 导致饥饿；释放读取后能正确采用更新回执，旧回执仍被拒绝，无基线倒退。

### R05-07：保护不能变成静默覆盖

保留并增强：两实际工作副本同目标别名冲突、不同 BindingRevision、匿名 producer、外部文件修改、中间有其他来源成功写入后原旧前提再次保存。

期望：本修复不能让这些请求仅因 Session 曾写过目标就继承不属于它的版本；冲突仍结构化报告，文件不被未授权覆盖。

### R05-08：有界记录、失败空洞与 Unknown 不退化

重复有限容量下 request/publish/adopt/ack，覆盖取消和编码失败、Unknown 的 writer 未退休、确认退休后继续。

期望：records/lane/provenance 都有说明清楚的界限与回收点；原容量背压、Unknown 禁止越过、Export Copy 不改基线、Save As 历史和高水位保持全部不回退。

此场景不新增性能提升指标，也不要求新后台线程或无限队列。

## 7. 实施顺序与交付

1. 核对当前分支、祖先关系和工作区。固定 `5866f990...` 为本轮失败基线，不 reset 或覆盖用户后来改动。
2. 阅读本文件、原 P05 和启动补充；沿用当前目录与唯一施工账本。
3. 先只加 R05-01、R05-02、R05-04 的真实回归，在固定原生产实现运行，保留失败退出、栈、返回值和文件内容。其余组合不要求修复前全部失败；如实记录。
4. 先修 B01 调度生命周期，再修 B02 衔接，或使用独立的清晰实现提交；二者最终都必须成立。
5. 在最终实现 SHA 显式 P05 门禁，全量构建与二次无工作，原 104 项和断言、三模型/R1、真实 IO、实际依赖负例继续跑。
6. 重装 SDK，原九组消费者重建执行；加入安装后真实持久化 W1 确认/延迟采用/W4 的使用验证。公共类型如有调整，同步所有真实消费者与既有头安装约定。
7. 冻结 `dev_log/P05-R1/`，实现与验收提交分开。保留原 P05 快照；历史核验按各自 implementation_sha，而不是把今天的源码当过去版本。
8. 正常推送 `codex/editor-redesign-v4`，停在 P05 等待复审；不 force-push、不修改 main、不进入 P06。

阶段结果只允许 PASS/PARTIAL/BLOCKED。新增必要场景未运行、仅本包隔离探针通过、到期旧路径残留，都不能报本轮 PASS。

C01 保持 P09/P12，C03 保持 P11，C04 保持 P12；其原 FAIL 和日志不变。B01 是新 SaveService 的回调生命周期，不是旧命令系统 C03 的延期项。

## 8. 可直接发送给实施方

> P05 主体认可，暂不进入 P06。只执行 P05 R1 的两项补正：SaveService 回调撤销/递归采用的记录生命周期，以及 WriteCoordinator 在已确认旧记录后保持合法同源版本衔接。
>
> 先用真实 SDK 保存角色和模型复现 describe 自撤销、accept 递归采用并确认，以及 W1 已采用确认/W2-W3 已发布未采用/W4 新请求。保留修复前结果，不能用隔离 shim 当真实引擎验收。
>
> 在现有服务中落实注册可调用性复查和执行期防重复、防删除；在现有 coordinator 中用有界状态保存衔接依据，不永久保留历史回执、不无条件继承文件当前版本、不删除冲突检查。保持所有 Published/Unknown 事实、FIFO、基线和容量契约。
>
> 不重写三类模型、History、SessionState、执行器、目录或包名，不新增平行协调器和通用管理器。最终显式 P05，重跑原 104 项、新 R05 回归、真实 IO、依赖负例和九组安装消费者，分别提交实现与 P05-R1 验收。C01/C03/C04 保持原 FAIL。推送后停在 P05。

## 9. 固定源码依据

以下链接固定到本次审阅验收提交，不随分支后续变化漂移。

- [P05 验收说明](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/dev_log/P05/README.md)
- [SaveService.cpp](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/src/SaveService.cpp#L143-L369)
- [SaveSource.hpp](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/include/lux/engine/editor/persistence/SaveSource.hpp)
- [SaveService.hpp](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/include/lux/engine/editor/persistence/SaveService.hpp)
- [WriteCoordinator.cpp](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/src/WriteCoordinator.cpp)
- [SaveTypes / 采用与发布事实](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/include/lux/engine/editor/persistence/SaveTypes.hpp)
- [持久化模块约定](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/README.md)
- [真实三模型保存测试](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/tests/persistence/models.cpp)
- [Coordinator 原测试](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/persistence/test/write_coordinator.cpp)
- [ExecutionRuntime 绑定](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/adapters/project_io/src/SaveExecution.cpp)
- [真实 ProjectArtifactStore](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/adapters/project_io/src/ProjectArtifactStore.cpp)
- [Scene Save As 角色](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/tools/scene/persistence/src/SceneSaveSource.cpp)
- [Material Save As 角色](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/editor/tools/material/persistence/src/MaterialSaveSource.cpp)
- [104 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/5866f990f8a5c4d68e19e0e395c9487c6c0f2779/dev_log/P05/logs/ctest.log)

原范围来自会话已提供的 `P05_persistence.md` 与 `LUX_ENGINE_P05_START_AFTER_P04_R1_2026-09-29.md`。本文件是其失败组合补充，不改写历史规范和收据。
