# LuxObject、信号与事件

`object` 是不依赖 UI、Scene 或 meta 的基础库。普通业务对象继承 `LuxObject`；它提供线程归属、非拥有父子链、同步事件及模板信号。父对象不删除子对象，固定成员或 `unique_ptr` 负责 RAII。

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

测试 `object.tree`、`object.queue` 覆盖父链、路由、RAII、重入增删、端点先销毁、异线程断连、槽复用、FULL/CLOSED 和载荷准备失败。`ui.cost` 单独测量实际对象实现的分配和热路径成本。
