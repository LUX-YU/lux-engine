# 抽象、C++20 concept 与动态边界施工规格

本章展开原设计，不引入第二套框架。代码分为“可直接按现有算法整合的契约示例”和“可选设计”，不得把示例中的替代类型复制成新的生产类型。

## 1. 机制选择表：先问变化在哪里

| 实际问题 | 采用 | 不采用 |
|---|---|---|
| 稳定唯一实现，只需隐藏内部 | 普通类API/private/PImpl | 自动配一个I类和转发Adapter |
| 编译期已知多种类型共享算法 | 小函数模板＋concept | 整个服务树模板化 |
| 封闭的运行时绑定集合 | 现有variant＋穷尽visit | any＋字符串tag＋到处downcast |
| 运行时未知类型异构拥有 | 现有窄virtual或一次owning erasure | virtual外再套多层function表 |
| 已提交事实/控件事件 | LuxObject TSignal＋Connection | 第二observer/event bus |
| 当前调用栈内的行为借用 | 具名callable/template/function_ref | 捕获进下一帧仍用借用函数 |
| 跨帧可调用工作 | 已有Task/角色/owning callable | 用function_ref规避分配后悬垂 |

concept不保证性能，virtual不自动构成设计缺陷。必须说明实际调用频率、开放集合、对象寿命和编译依赖。

## 2. 必做：现有 deliverInput 的局部约束

### 2.1 目标文件和可见性

真实目标：`editor/workbench/sinclude/lux/engine/editor/workbench/InteractionDelivery.hpp`。

仅为Material/Flow具体UI实现的同步交付核。两个target以精确PRIVATE include路径消费；不把所有workbench sinclude传到SDK，不让authoring或widgets使用它。C++ namespace改为 `lux::editor::workbench::detail`；旧editing路径及using别名全部删除。

### 2.2 契约头示例

以下类型名用于该私有实现，不是新的公开ABI。生产Result继续采用两个View现有的 `lux::cxx::expected<void,E>`。不要求改原错误enum。

```cpp
#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace lux::editor::workbench::detail
{
    template<class R>
    concept VoidDeliveryResult =
        std::default_initializable<R> &&
        std::move_constructible<R> &&
        requires(R& result, const R& observed)
        {
            typename R::value_type;
            typename R::error_type;
            requires std::same_as<typename R::value_type, void>;
            { static_cast<bool>(observed) } -> std::same_as<bool>;
            { result.error() } -> std::same_as<typename R::error_type&>;
        };

    template<class F, class R>
    concept DeliveryAction =
        std::invocable<F&> &&
        std::same_as<std::invoke_result_t<F&>, R>;

    enum class EInputDeliveryStage : std::uint8_t
    {
        BEGIN, PREVIEW, COMMIT, CANCEL, COMPLETE
    };

    template<class Validate, class Cancel,
             class Begin, class Preview, class Commit>
    requires std::invocable<Validate&> &&
        VoidDeliveryResult<std::invoke_result_t<Validate&>> &&
        DeliveryAction<Cancel, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Begin, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Preview, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Commit, std::invoke_result_t<Validate&>>
    auto deliverInput(
        EInputDeliveryStage& stage,
        bool commit,
        Validate&& validate,
        Cancel&& cancel,
        Begin&& begin,
        Preview&& preview,
        Commit&& finish) -> std::invoke_result_t<Validate&>
    {
        using Result = std::invoke_result_t<Validate&>;
        if (stage != EInputDeliveryStage::CANCEL &&
            stage != EInputDeliveryStage::COMPLETE)
        {
            if (auto result = std::invoke(validate); !result)
            {
                return result;
            }
        }
        if (stage == EInputDeliveryStage::CANCEL)
        {
            if (auto result = std::invoke(cancel); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::BEGIN)
        {
            if (auto result = std::invoke(begin); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::PREVIEW;
        }
        if (stage == EInputDeliveryStage::PREVIEW)
        {
            if (auto result = std::invoke(preview); !result)
            {
                return result;
            }
            stage = commit ? EInputDeliveryStage::COMMIT
                           : EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::COMMIT)
        {
            if (auto result = std::invoke(finish); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        return Result{};
    }
}
```

这是按当前已读函数整理的**整合示例**；实施者必须在真实lux-cxx版本和实际Material/Flow Result上编译。若当前Result正确但traits不符，调整概念到真实必要能力，不能改全部Result或添加桥只为满足示意代码。

不机械加入 `noexcept`：现有copy/载荷转换的异常政策必须保持，只有真实不抛契约才用requires noexcept。

### 2.3 语义律（编译器不能代替的部分）

- 每个action只借用当前调用栈，函数不保存它们。
- 调用者在产生请求时固定based_on；本函数不重新取current，不拥有或复制作者模型。
- action失败返回原Result，失败阶段不前移；成功阶段不因下一阶段BUSY重复执行。
- validate不是永久授权。后续action跨扩展回调或内容边界时，仍用原领域gate/expected检查。
- CANCEL/COMPLETE沿原规则处理；不得把所有终态无条件重新begin。
- payload析构、失败队首清理及oldcode保活依旧由原具体View/interaction负责。
- 普通Preview不写作者编码；一次Commit产生一次历史。

### 2.4 正负例要求

正例：实际Material UI actions和Flow UI actions，分别编译运行原 `draft_source_*`、BUSY阶段恢复、取消、stale、Undo/Redo路径。

负例至少涵盖：cancel返回bool；preview返回不同的expected错误类型；action只支持&&却在同步借用&上调用；返回`expected<int,E>`而非void。先通过相同头的正例，再要求负例在约束处失败。错误文本不必绑定编译器逐字措辞，但必须能定位目标约束，不能是缺头/包。

本私有头无需为了编译负例安装。编译契约test使用明确内部路径；安装消费者走公开View并链接实际实例化。两类证据分别记录。

## 3. 可选：FrozenEncoder 的静态复用

只有真正共用流程时实施。原设计的FrozenEncoder只表示：`codec.encode(const Snapshot&,limit,stop)`返回现有PersistenceResult<EncodedArtifact>，snapshot提供对应ContentStamp。

不要求添加无用 `SceneCodecAdapter/MaterialCodecAdapter`，也不要求让三种不等价的格式硬统一。优先直接约束实际可调用codec函数对象，在领域实现CPP实例化，再交给现有OwnedEncodeJob。

需明确：
- 最外层code owner、snapshot/codec的成员声明顺序及移动赋值；
- 原消费输入在READING内清理的保证；
- frozen bytes的budget语义和SharedBytes拥有关系；
- stop/error不变成成功；
- job采用基线仍在owner，不在worker；
- source-save与derived publication不是同一来源链。

若共享只剩三行调用，保留原具体实现更简单。L5记录“不引入此概念”的理由是合规结果，不因此阻塞；不得将可选项扩张为新框架任务。

## 4. 动态边界保持窄而真实

### 4.1 IEditSession

Store需要管理运行时未知Session，保留虚析构/描述/关闭私有契约。动态角色不需要暴露所有领域编辑，具体工具继续使用typed SessionKey及公开领域API。

### 4.2 ISaveSource / IEncodeJob

SaveService不知道具体Scene/Material/Flow仍可接收角色。code与payload生命期由原OwnedEncodeJob/registration承担。一个动态边界足够；不增加IJobPort→Adapter→Facade→IEncodeJob。

### 4.3 ViewFactory / 插件

未知工厂使用当前必要多态；concept只在源码扩展的编译期检查，不是插件ABI。P11才完成动态注册/版本生命周期，当前不伪造成功的PluginManager占位。

C ABI/稳定类型擦除若已有真实用途，不因“禁止Adapter目录”而删除。禁止的是长期同义体系互相补偿，不是实际跨ABI边界。

## 5. 实例化放置与公共依赖

- generic定义只include必要正式值/Result和标准traits，不include全部具体Session/codec。
- 具体实例化位于对应活动provider或工作台CPP；application只选择组合，不承担所有内置模板实现。
- 内置组合若显式实例化能实际降低编译负担可采用；不能限制合法外部源模板扩展。
- 若必须看所有后端才能编译策略头，依赖倒置没有成立；不能以“模板会优化”掩盖。
- 公共PImpl头只保留真实值/Result所需完整类型；引用/指针服务采用前置声明。不要为每个前置声明再建一个forward-only API库。

## 6. C++20与基建纪律

固定C++20，不无条件引入std::expected/std::move_only_function/std::scope_exit等更晚库接口；用当前已验证lux-cxx设施。不使用std::span/string_view保存悬垂借用，不跨frame缓存Registry/Pane裸引用。

新concept/type的完成条件是：真实用途、语义律、consumer/实例化点、编译与行为证据、未引入多余依赖。没有这些就应删除，而不是将其当未来可扩展性资产。
