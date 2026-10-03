# 06　持续规则、架构反思与交付格式

## 1. 判断架构清晰，不能只看五个目录

最终文档对每条主要行为画出实际调用链及唯一 owner：

- 资产 read/decode → native result → script handle → resume → release；
- ProjectUpdate → fixed plan → prepared reservation → publication → adoption；
- configuration draft → prepare → Session 的实际领域提交；
- compiled result → target adoption → RenderResources → retirement；
- activity state → read-only view → command/request → original owner。

每条链都回答：数据从哪来、能否修改、谁允许执行、失败是否有副作用、何时失效、谁最后释放。

如果一次普通查询必须穿过 Application→Manager→Adapter→Provider→另一个 Manager，而各层没有独立权限/状态/表示职责，则收敛。若这些层分别承担线程运输、领域准入和资源采用，就保留边界，优化真实数据工作而不是只追求短栈。

## 2. 数据/执行者的命名判据

| 形态 | 正确示意 | 禁止机械修法 |
|---|---|---|
| 纯数据/描述 | `plan.files()`、`snapshot.find(id)` | 给每个 getter 配 Reader。 |
| 纯转换 | `decodeModelRecipe(bytes)`、`prepareSceneConfiguration(draft, descriptors)` | 用只含 static 的 Processor/Operation 空壳。 |
| 执行活动 | `operation.advance()`、`executor.undo(history)` | 两套同义状态机并存。 |
| 资源拥有 | `host.adopt(view)`、`scope.requestStop()` | 把权限和生命周期拆成外部可改 bool。 |
| 语言表示 | `makeScriptAbilityLuaContribution<Ability>()` | 在 Lua callback 中复制 native 算法或生成任意方法反射总线。 |

EC1 的 History 分离是已落实的具体决定，不是要求所有 State/Store 都变成 POD。真正带 invariant 的数据允许 private storage、构造/观察及必要 RAII。

## 3. 用类型减少冗余判断，但保留真实边界

每个删除/合并检查条目包含：首次证明点、复用范围、可能失效事件、公开/私有入口、替代类型和回归。

可以收敛：同一个 immutable plan 的多次字段组合校验；无回调的同步段反复 find；同一版本稳定目录多消费者复制。

必须保留：外部脚本值 decode，能力 schema 与绑定身份，跨 await 的实例/资源代次，外部文件发布前版本，codec/插件回调后的状态，旧完成对新目标的采用验证。

没有 callback/await 不自动保证线程安全，仍按具体 owner 合同。`completion.active()` 是事实查询，不是锁，也不保留未来有效性。

## 4. C++20 和静/动态绑定

- 继续 `lux::cxx::expected`，不引入未声明的 C++23 标准库设施。
- concept 用于 codec/输入布局/typed completion/真实共享算法的语义表达；不能证明运行期有效域、原子提交或数据深不可变。
- 运行时开放的插件/能力在已有边界擦除一次；不能用封闭 variant 冒充未来所有扩展。
- 数据结果封装关联；可复制只能复制不可变共享数据，不能复制取消/发布/退休责任。
- 固定地址对象禁止复制/按值移动，拥有单元 move 后原对象无重复释放。资源值替换先清旧 payload 再清 code。
- 限定模板在真正使用处实例化；不把整个 EditorApplication/Runtime 模板化。
- `function_ref` 只在当前同步栈借用；异步或存入对象的闭包用合适 owning callable，并明确捕获寿命。

## 5. AGENTS 对照（修改闭包内必核）

| 规则 | 实施检查 |
|---|---|
| 120 列/语义换行/4 空格 | 不用过度机械拆行制造多层噪音；长返回值先给准确别名。 |
| PascalCase / E / V / private 尾下划线 | 新/修改 public 类型及 intent/value 一致；不再出现 Data 名掩盖执行。 |
| 具名 bool 与短路 | 独立条件按语义组命名；指针/范围/算术先建立安全前提。 |
| include/sinclude/pinclude | 外部插件只有真实安装契约；不拿私有路径充当 SDK。 |
| 数据组件只包含数据依赖 | Script/Feature/系统行为不进入组件/资产描述头；共同编码归原下游值。 |
| 异常 | 语义失败用 Result；仅规定的 foreign/factory/codec 容纳边界处理异常；不向 dispatch/drain 插 catch。 |
| Lua 错误/yield | 沿原 C boundary，不跨未析构 C++ owner；不以 noexcept 当作不会失败。 |
| Observer | 连信号折入存量；结构修改在原命令屏障；嵌套入队留后批；需通知的字段通过 patch。 |
| 诊断 | 结构化错误保留；库不决定终端出口，单一宿主 sink。 |
| 构建 | all/-j4/-k0、两轮无工作、无并发实机；真实 provider 声明；模块头三 prefix 同步。 |
| 删除 | 旧入口/alias/generated/install 残留一起清；只读旧资产迁移和历史证据保留。 |

标准 Lua 手册只作为 C API 边界背景；本项目实际 Lua55 补丁、ABI、semantic 与 generated binding 才是执行依据。不要联网更新依赖或更改 Lua 版本来绕开本轮问题。

## 6. 架构上的“保留”也必须写进报告

以下不需要成为新全局类型：ScriptManager、GameApiContext、UniversalResourceHandleStore、EditorServiceLocator、ProjectPublisher2、PreviewCoordinator、WorkspaceWriter、ConfigurationProcessorFactory。

`ScriptAssetScope`（目标）若确实新增，其责任只限 **此原生脚本实例的已接受请求和结果保管**，不是系统级缓存、线程池或未来万能所有者。能复用现有实例资源域就不另建。

编译错误与预览错误、作者源与 cooked bytes、文件效果与目录采用、能力可用与即时准入，继续明确区分。这些区别不是冗余。

## 7. ABI 与版本

- EC1 Editor V8 保持为当前事实，不回写 V7。Editor public 类型改变才更新实际 ABI 指纹及对应消费者。
- 新 script method/value schema 为独立稳定版本。加入普通方法贡献不等于需要更改通用 VM ABI。
- 如果补 per-instance prepare/revoke 的原合同需要变更现有结构布局，更新真正受影响的 Script ABI 并证明旧输入在调用前拒绝；不要一并无故改 Runtime 插件 ABI。
- 项目 manifest v3/旧版本只读迁移保持；此次职责重分配默认不改变磁盘格式。
- 稳定 ID 的规范名/哈希/版本不因 C++ 文件移动擅自改变。

## 8. 每批交接字段

在现有账本 ec2 节点记录，不创建第二账本。至少包括：

```text
batch / input_sha / current_sha / dirty_status
source_files_and_symbols
responsibility_before -> responsibility_after
removed_entries / retained_entries_and_reason
owner_and_lifetime_changes
native_and_script_contracts
invalid_states_eliminated
checks_removed_and_validity_proof
tests_executed_with_commands_and_exit_codes
inherited_evidence_with_original_sha
not_run_or_waived
next_entry_and_known_blocks
```

机器清单可用本包 [IMPLEMENTATION_MAP.json](IMPLEMENTATION_MAP.json) 作为种子；它不是已完成文件清单，R0 必须补全真实消费者。

## 9. 最终验收记录格式

```text
stage: EC2
implementation_sha: <完整最终实现>
acceptance_sha: <独立记录提交>
baseline: 248adc4576943cab83976afd8d1d5f31b63b70a9
status: PASS_WITH_DECLARED_SCOPE / PARTIAL （按实际决定）

A. EC1 继承：已复核/未重跑，准确 SHA
B. 职责迁移：原算法、唯一新位置、旧入口删除
C. 游戏脚本：实际 exported capabilities 与明确未绑定项
D. C++ / Lua / PLAYER / Editor Run 真实链路
E. 依赖、生成、安装、AGENTS 检查
F. 性能与生命周期：计数/样本/覆盖范围
G. 免验、NOT_RUN、未完成项
H. 工作区、用户补丁、main/分支/发布状态
```

不得用一句“全绿”覆盖 A–H；也不需要为保留范围建立大量额外胶水文件。

## 10. 施工结束的判据

真正可复用能力不再被控件或 AppImpl 私藏；固定数据不夹带隐含可变权限；C++ 与脚本共享真实执行，脚本边界有完整寿命；正式产品和 PLAYER 可运行；旧入口实际退出。

达到这些目标后停下复审，不因为还能设想更多 API 就再扩展阶段。未来物理/音频等完整绑定按具体用户需求推进，不在本轮自动追加。
