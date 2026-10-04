# EC3-A：身份、模块声明与单份描述存储

## A1. 已确认的现状，不要误读为目标设计

`CommandDescriptor` 当前拥有 CommandId 和三个 `std::string`；Entry 按值保存它。`CommandRegistrySnapshot::find()` 当前逐条执行 `entry->descriptor().id.name() == id.name()`；快照创建用嵌套循环排重。[S03,S04,S06]

本次读取的 lux-cxx 参考版本中，`StableNameIdView` 已缓存 constexpr FNV-1a64；相等运算仍为 hash 与 name 双重检查。拥有型 `StableNameId::view()` 调用 `fromVerified()`，会重新校验名字哈希。不能因为类型名字包含 hash 就假定每次调用没有读字符。[S05]

这个参考版本来自在线仓库，不是用户机器当前安装资格。历史 P10Q 明确用过不同的 lux-cxx SHA；C0 必须核对实际源、三个 include 前缀、工具包和 TypeToken。不得为了适配本文示例而降级依赖。

## A2. 区分名字、稳定身份与本次调用句柄

| 层面 | 用途 | 是否可以读取字符串 |
|---|---|---|
| 规范名 | 插件注册、文件恢复、日志和外部文本入口 | 可以；是冷路径 |
| 预计算稳定身份 | 在同一身份命名空间内建立索引 | 比整数；不得把 hash 当密码或权限 |
| CommandHandle | 固定条目、版本和代码寿命 | 直接访问条目，不重新按名称查找 |
| 显示文本 | label、分组、用户搜索、翻译 | 可以；不是业务分派键 |
| 领域目标 | SessionId／ViewId／ContentStamp 等 | 继续用现有完整身份与代次 |

本轮不将不同语义域的 uint64 混用。命令、视图、设置、源格式身份有各自的 tag／已有强类型。一个 hash 不能代替 SessionId、文件摘要、资产魔数、输入版本或代码 ABI。

规范名继续可持久化。不要把旧配置中的稳定名字改成指针、进程内槽位或 `std::hash` 的不稳定结果。国际化 label 和用户重命名也不能改变命令身份。

## A3. 注册时验证，派发时不读业务名称

固定声明：使用现有 constexpr 字符串／预哈希视图。常量必须在编译期形成，不在每次 query 里构造拥有型 CommandId 再 `.view()`。

动态声明：在接受整个候选目录时规范化并计算一次。验证顺序如下：

1. 验证规范名非空、编码、长度、身份域、版本和完整描述。
2. 重新计算或验证输入携带的 hash；不信任插件自己填写的数值。
3. 建立候选 hash 索引，检测重复。
4. 同一名字重复注册按原重复／替换政策处理，不能混成 hash 冲突。
5. 两个不同名字同 hash：返回明确 HASH_COLLISION 类诊断，整个候选不发布。
6. 验证 callback、参数类型、shortcut、owner 和容量；成功后按原批次协议共同发布。

外部文本入口只在解析／激活边界比较规范名，取得已解析 handle；任意文本不能只取 hash 后直接命中另一个同 hash 条目。该边界可以保留一次精确比较，不能为满足“零字符串比较”破坏正确性。

有限宽度哈希对任意扩展集合不可能保证绝对无碰撞。这里选择明确拒绝碰撞，换取活动目录内的纯数值查找，而不是在每个热查找中做字符串回退。

不要修改 lux-cxx 全局相等规则。相同数值的目录外 raw hash 不是已验证的命令权限。原 API 如果接受 `CommandIdView`，把它明确列为身份解析边界；正常 UI／快捷键和程序化循环持有解析后的 handle。不得每次转成 string 再调用 resolver。

## A4. 直接扩充原快照的索引，不建立第二个注册器

在 `CommandRegistrySnapshot::Data` 中添加一次构造的数值索引。首选紧凑排序 `{hash, entry_index}` 数组，冷路径 O(N log N)，热 lookup O(log N)；如实际容器基准支持现有 lux-cxx hash container，允许替换，但不是本轮必须另写 hash table。

保留 `entries` 的展示／装配顺序。不要为了索引把用户菜单顺序隐式改成 hash 排序。

该索引只是原不可变目录的派生索引。不得复制 CommandEntry、字符串、回调、代码 lease 或第二套 revision。快照替换时索引与条目共同发布。

已有 CommandHandle 直接引用条目，PINNED 调用不需要新 slot map。CURRENT_REGISTRATION 在出队时用固定稳定 ID 查当前数值索引，再验证原 scope／参数／input_version；不能把旧请求重绑成任意新接口。

### 跨目录版本的同哈希不同名字

仅检测同一候选集合还不够：旧队列可能持有 A，而新的活动目录已移除 A 并加入同 hash 的 B。CURRENT_REGISTRATION 不能因此执行 B。

原 handle 固定其规范身份；当 registry revision/entry 改变需要重新解析时，必须验证当前条目仍是同一规范身份，或者使用已有的经过验证的 intern identity。默认采用**版本切换的冷重解析**：hash 定位后一次精确身份核验，再建立可复用的当前 handle。正常未变版本的派发不做该字符串工作。

持久配置、快捷键文件和外部文本同样走冷解析；不能将只存 hash 的旧来源直接认作当前名称。若使用 intern，必须属于原 registry 的有限身份管理，不是跨项目全局符号池，并记录旧 handle 释放与容量回收。没有必要不新增 intern。

这项正确性开销单独统计为 cold resolution，不能从性能日志隐藏。不要为宣称任何时候零比较而接受错误命令。

如果为 UI 事件增加一个紧凑 locator，它必须具有目录域／代次或仍由原 handle 锚定，不得缓存 vector 地址并跨发布使用。没有实际需要则不增加该类型。

## A5. 同一种 CommandDescriptor：literal 与动态来源共享 schema

撤销旧建议中的长期 `CommandSpec + materializeCommand()` 双份结构。

目标形状如下。它是本阶段目标代码，不是当前已编译接口：

```cpp
struct CommandDescriptor final
{
    CommandIdView id;                  // 复用 lux-cxx 的预哈希、tag 化名称视图
    std::string_view label;
    std::string_view group;
    std::string_view shortcut;
    ECommandScope scope{ECommandScope::APPLICATION};
    std::uint32_t input_version{1};
    cxx::TypeToken argument_type;
};

inline constexpr commands::CommandDescriptor exit_command{
    commands::CommandIdView{"lux.editor.exit"},
    "Exit",
    "File",
    "Alt+X",
    commands::ECommandScope::APPLICATION
};
```

沿用原类型名、公开逻辑 include 和语义字段。不要在 Editor 复制另一套 FNV、FixedString、TypeToken。若实际 TypeToken 的 constexpr 支持不足，先在正确的 lux-cxx 基础处补齐或采用已有 literal token；不以增加第二个 CommandSpec 绕过。

CommandDescriptor 此后是一份非拥有描述视图。借用不是默认安全：必须与下一节的 Entry 存储契约一起迁移，不能只把 `string` 搜索替换成 `string_view`。

## A6. 静态、插件静态、动态描述的三种寿命

### A6.1 固定内置描述

实际模块在 `.hpp` 或 `.cpp` 声明 `inline constexpr/static constexpr CommandDescriptor`。若外部调用方需要类型安全引用，就放最窄公共语义头；仅模块内使用则留 CPP／pinclude，不能强迫所有模块 include 总表。

Entry 引用同一 descriptor，不为四段文本申请堆内存，也不把它再 materialize 成拥有型描述。可用引用非类型模板参数绑定固定声明，保证调用方不能传临时 descriptor：

```cpp
// 目标示意；实现应复用原 Entry / callable，不增加执行期转发。
auto entry = bindCommand<exit_command>(code, query_exit, execute_exit);
```

这只是创建时的静态绑定辅助。命令执行仍走原 Registry→实际 callable。

### A6.2 插件内的静态描述

同样直接引用插件静态 descriptor，但原 CodeLease 必须覆盖描述、文字、callback、payload、析构以及控制块清理。不能假定静态文字在 DLL 卸载后仍可用。

检查现有 `pinCodeOwner` 和弱控制块回归，不重写已验证的代码保活链。不允许插件用 builtin lease 伪报其来源。

### A6.3 运行时生成的描述

动态工具菜单、插件产生的 label、运行期默认值：需要一次拥有型 backing。复用已有 immutable owner／SharedBytes 等基础，在私有 `DescriptorStorage` 中最终冻结全部字符和同一 CommandDescriptor。

冻结前可以用局部 string 构造；所有增长、移动完成之后再建立 string_view。不能指向构造器临时 string、即将 reallocate 的 vector 或可被 settings 页面改写的缓冲。

Entry 只保留 descriptor 指针／引用与可选 backing owner；静态路径无需 backing 分配。动态路径不再保留一份源 Spec 或一份额外拥有型 Descriptor。

允许两种内部实现：原 Entry 持指针和可选 owner；或引用同一 immutable allocation 中的 descriptor。C0/C1 对比实际 `sizeof`、Entry 分配和间接层数后选择。不得为了省一个 owner 指针引入悬空借用。

public `descriptor()` 返回借用，其有效期绑定 handle／entry／snapshot。需要跨期持有者保留现有 handle，不裸复制 string_view。

## A7. 哪些成本可以消除，哪些必须承认

| 项目 | 目标 |
|---|---|
| 固定命令元数据的文本堆复制 | 0；引用模块静态文本 |
| 固定 descriptor 的第二份长期 schema 对象 | 0；Entry 引用原声明 |
| 运行期动态文本的物理内容副本 | 一份冻结 backing；不按每个消费者复制 |
| Entry／callable／CodeLease／索引 | 有实际用途，不能宣称零成本 |
| UI 的树节点、状态与个性化 label | 可以存在；它们不是另一份业务目录 |
| 每次已解析命令派发的按名称扫描／重新哈希 | 0 |
| 外部字符串第一次解析与注册碰撞验证 | 允许并单独计数 |

不要预写“节省 X 字节”。在实际 ABI、MSVC/STL、32/64 位条件下记录 `sizeof(CommandDescriptor/CommandEntry/CommandHandle)`、静态数据量、字符容量、控制块及索引成本。

默认不为每个 string_view 增加 shared_ptr。一个 entry 或 snapshot 的 owner 保持整份描述足够。字符存储不重复，不等于 schema record、索引、引用可以不存在。

## A8. 菜单与快捷键：复用描述，不重新建字符串业务表

CommandMenu 保留固定 handle 与输入。UI 层不能 include Editor command 实现；使用共同的现有 UI CommandIdView 与明确寿命的菜单观察。

读取当前 MenuItem 契约后选择：让 CommandMenu 以原快照保活轻量菜单描述；或者在现有 UI 菜单模型中补中性的 source owner。不要为每一项复制 descriptor 的全部字符串，也不让 Root 借一个临时 span。

菜单布局可以保存自己的树和 group 节点，但只在目录／有效菜单配置 revision 变化时重建。用户翻译或重命名的显示文本与固定 canonical ID 分开存储。

当前 validShortcut 与 CommandMenu::shortcut 存在两段相近语法处理。[S03,S07] 提取一份中性解析规则放在实际共同 provider；复用原按键类型，不复制第二个 EKey，也不能让 activities/commands 链接 ImGui。

固定默认快捷键在装载时解析；用户覆盖在设置提交时解析，键盘事件不再解析 `"Ctrl+..."`。明确无绑定、未知按键、重复修饰、多个命令冲突、平台等价键等结果。保留当前支持的键集合，不靠静默回退为 key=0 消化错误。

group 的 `File/Submenu` 文本只在菜单编译边界解析。稳定 UI 帧不得重新切割所有 group 或逐名字匹配目录。

## A9. 三类声明的统一范围

命令的同型策略优先完成。ViewFactoryDescriptor、SessionKindDescriptor／SourceAuthoring 使用同样的 lifetime 原则，但必须优先复用已有类型和常量，不能再建对应 Spec/CompiledSpec 套餐。

固定 `content_kinds`、extensions 等数组可用 module-static array/span；动态集合由一份 immutable backing 保活。Entry 保留类型版本、默认关系和 CodeLease。

PaneTypeId 与 ViewTypeId 的既有同一语义不应拆开；SessionKind 与 AssetType 不应错误合并。新插件不需要修改宿主中央枚举。

35 个固定命令 ID、12 个固定 view、3 个 source 描述是初始必须迁移的集合。C0 扩展核查同类生产注册；逐条标注迁移、已符合、动态规则或确实不适用，不把正则命中数当成完成数。
