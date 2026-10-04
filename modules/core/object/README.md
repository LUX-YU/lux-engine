# LuxObject、信号与事件

`object` 是不依赖 UI、Scene 或 meta 的基础库。需要身份、线程归属或通知的业务对象继承
`LuxObject`；普通数据不要求继承。它提供两态父子链、同步事件及模板信号。

成员、栈对象和外部智能指针保持 `EXTERNAL`。派生组合类型通过 protected `attachChild()`
建立非拥有关系，或通过 `adoptChild(unique_ptr<T,D>&&)` 转交真实删除责任，成为 `PARENT_OWNED`。
接管失败不移动候选和 deleter；支持普通 `T*` 指针及可无失败移动的 deleter。UI 使用自己的结构入口。
父对象销毁时，托管孩子按逆接管顺序回收，外部孩子只解绑。固定成员无需额外 heap owner。
孩子若借用派生成员，派生析构体必须先 `clearChildren()`；基类析构不执行保存等业务。
机械回收使用兄弟链的迭代后序遍历，不逐层重新扫描整棵子树，也不随树深度增加析构调用栈。
接管时仍检查祖先以拒绝成环；这次局部准入检查不需要为每个对象维护第二份层级索引。

`requestDestruction()` 只记录托管对象的销毁意图。原 ObjectState 标识合并同一对象的请求，
不会因地址复用而销毁新对象。宿主在业务派发和 UI 遍历外调用 `ObjectMessageQueue::collectRetired()`；
它处理固定批次，期间新请求留到下一批。外部成员不能独立请求删除，消息队列 FULL 不丢失回收责任。

`shareOnDispatcher()` 转移真实唯一 allocation 到共享控制块。最后引用可在 worker 释放，但回收节点
在创建时就已准备，实际析构回到 dispatcher 的 owner 安全点。消息关闭后仍可收取退休责任；
queue provider 的析构是最后一个 owner 安全点：关闭消息准入，分批收回已经交还的对象，不泵业务消息、
不等待任务。若仍有外部共享 owner、活动回调或错误线程使回收无法推进，继续报告寿命契约错误。
移动状态型 deleter 时仍处于对象的原结构保护内；拒绝准入不消耗调用方的候选或最后代码 pin。
`CodeLease` 和 `pinCodeOwner()` 位于本模块；共享控制块及释放桥由本库编译，代码保活覆盖对象、
deleter 清理及返回，过期 weak 引用不必继续保活插件。

```cpp
class Counter final : public lux::object::LuxObject
{
public:
    using LuxObject::LuxObject;
    lux::object::TSignal<int> changed{*this};

    void setValue(int value) noexcept
    {
        if (value_ == value) return;
        value_ = value;
        const auto delivery = emit(changed, value_);
        // 宿主按自己的诊断出口处理 delivery 中的失败；部分广播不能整体重发。
        accountDelivery(delivery);
    }

private:
    void accountDelivery(lux::object::SignalDelivery) noexcept;
    int value_{};
};

auto connected = lux::object::LuxObject::connect(&a, &Counter::changed, &b, &Counter::setValue);
if (!connected) return lux::cxx::unexpected(connected.error());
value_connection = std::move(*connected);
```

信号是实例成员。无参数使用 `TSignal<>`；不需要 CRTP、编号、反射标注、静态定义或代码生成。`emit` 是 protected，只能发送当前对象自己的信号。Payload 按同类型值或 const 引用传入，回调返回 void 且必须 noexcept。成员信号指针支持正常 C++ 继承调整。

`Connection` 是唯一的 move-only RAII 凭据：析构或 `disconnect()` 取消，移动赋值先取消原连接。调用方须保存成功结果。发送方或显式接收方销毁也会取消；外部凭据随后析构仍然安全。lambda 捕获遵循普通 C++ 寿命，不自动跟踪任意捕获。

connect/emit 在发送方线程执行。DIRECT 要求显式接收方同线程；QUEUED 要求接收方 dispatcher；AUTO 按双方线程选择。无接收方 lambda 仅 DIRECT。异线程可断开 Connection，原子取消立即阻止后续接纳，容器回收在发送方安全点进行；disconnect 不是 join，已开始回调可以完成。

每信号惰性建立 `StableSlotMap`，使用 lux-cxx 的 `SlotKey`，发送只遍历该信号。最外层派发固定可见范围；期间新增连接下次可见，断开的未开始回调跳过。接收端反向链用于关闭，不是第二份订阅表。取消维护不使用可能 FULL 的业务消息；无 dispatcher 的发送方在下一次 connect/emit/关闭收尾。

`SignalDelivery` 分别报告 direct、queued、full、closed。排队载荷由消息工厂固定，实际执行时重查取消及接收端存活。队列不会读取已经销毁的对象。消息准备中的内存不足或载荷复制异常会终止进程。

同步 `EventView` 支持过滤和父链传播，与信号广播分开。当前对象及祖先不能在回调栈中析构；对象在自己的线程销毁，析构不泵消息。关闭业务和结束编辑须先于对象析构。没有公开 `ObjectWeakRef`，UI 的焦点和维护借用由 UI 自身的析构注销协议处理。

测试 `object.tree`、`object.queue` 保留原父链、路由和信号行为；`object.ownership` 验证混合树、
拒绝不消费、清理期重入、安全点和真实 DLL 析构尾部。独立 object-ownership SDK 消费者使用安装的
公共头和库重跑这些行为。测试入口的存在不代表已执行，实际结果以阶段验收记录为准。
