# LuxObject、信号与事件

`object` 不依赖 UI、Scene 或 meta。LuxObject 提供非拥有父链、同步事件和模板信号。

ObjectRuntime 在 object 动态库中定义进程唯一身份目录。宿主必须先显式初始化 ExecutionRuntime；
所有 LuxObject 在 ExecutionRuntime 的应用线程构造、访问和实际析构。ObjectRuntime 不决定线程归属。默认构造取得全局代际 ObjectId；注册表只保存非拥有地址。
resolve() 只提供一次同步借用，不能跨回调或跨帧保存。对象关闭后撤销身份与接收资格，槽位复用不恢复旧 ID。

setParent()/addChild()/removeChild() 仅修改关系，不转移删除责任。addChild 可原子移动到新父对象；
重复设置同一父对象成功但不改链。线程、环、关闭与活动结构资格验证失败时原树完整。
父对象析构只解绑剩余孩子；成员、unique_ptr、shared_ptr 决定实际析构。需要派生成员的内容必须在该资源
仍有效时由实际 C++ owner 清理。UI 的类型化入口执行自己的拓扑验证。

ObjectRuntime 没有消息队列、唤醒绑定、dispatchPending 或 collectRetired 接口。
QUEUED 信号和 ObjectTarget 回调通过 stdexec 调度到 ExecutionRuntime::mainScheduler()，
唯一应用循环调用 ExecutionRuntime::dispatch()。DIRECT 信号保持同步，不增加排队。
排队工作按生产者有序，不承诺跨生产者全局 FIFO，也不再提供固定容量 FULL 或双批次隔离合同。
ECS 修改仍必须在其既有结构安全点采用，不能把队列批次当作 ECS 隔离边界。

shareOnRuntime() 转移真实唯一 allocation 到共享控制块。最后引用可在 worker 释放；
预先准备的 Reclamation 记录嵌入标准 schedule Operation，不再加入 Object 独立回收队列。
正在执行对象回调时释放被延迟，实际析构在应用线程的后续安全点执行。
这条已有清理路径不需要新的 spawn 准入，ExecutionRuntime 停止期间仍会排空它。
所有对象与外部 Producer 必须在 ExecutionRuntime 析构之前结束；ObjectRuntime 静态析构不调用 Execution。
CodeLease/pinCodeOwner 的宿主释放桥覆盖对象析构、状态型 deleter 清理和返回；weak 控制块可晚于 DLL 卸载。
pinCodeOwner 只接受真实 unique_ptr allocation，在首次共享时建立代码 pin。之后复制返回的 shared_ptr，
不得重新包装已有共享 owner；该输入合同从根源禁止释放桥链，不通过 RTTI 查询或全局缓存解包。
普通 C++ 值在最后强引用释放的线程清理；LuxObject 的线程亲和回收仍使用 shareOnRuntime。
共享准入失败不消费候选或代码 pin，deleter 移动期间仍受结构保护。

`post(ObjectTarget, callback, code_lifetime)` 的 POSTED 表示接纳一次应用线程回调。
目标失效或 Runtime 停止时回调接收 nullptr，已接纳完成不能静默丢弃；拒绝时不调用回调。
停止之后的新投递返回 CLOSED。可选代码 pin 保持到 callable 销毁及返回 Core 调度循环之后。
ObjectScheduler 仍在有效对象借用内交付 set_value，失效或取消交付 stopped。
`whenEmitted(source, signal)` 等待一次事件，连接 Operation 时取得全局应用线程 Scheduler，
不再要求调用方另外提供一个可能指向不同线程的 delivery Scheduler。

```cpp
class Counter final : public lux::object::LuxObject
{
public:
    lux::object::TSignal<int> changed{*this};

    using LuxObject::LuxObject;

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

connect/emit 在唯一 owner 线程执行。DIRECT 同步交付；QUEUED 使用 Execution 主线程调度；AUTO 对同域对象同步交付。无接收方 lambda 仅 DIRECT。异线程可断开 Connection，原子取消立即阻止后续接纳，容器回收在发送方安全点进行；disconnect 不是 join，已开始回调可以完成。

每信号惰性建立 `StableSlotMap`，使用 lux-cxx 的 `SlotKey`，发送只遍历该信号。最外层派发固定可见范围；期间新增连接下次可见，断开的未开始回调跳过。接收端反向链用于关闭，不是第二份订阅表。取消维护使用记录内的 schedule Operation；connect/emit/关闭也会清理已取消连接。

`SignalDelivery` 分别报告 direct、queued、closed。排队载荷由类型化 Sender 拥有，实际执行时重查取消及接收端存活。队列不会读取已经销毁的对象。消息准备中的内存不足或载荷复制异常会终止进程。

同步 `EventView` 支持过滤和父链传播，与信号广播分开。当前对象及祖先不能在回调栈中析构；对象在自己的线程销毁，析构不泵消息。关闭业务和结束编辑须先于对象析构。没有公开 `ObjectWeakRef`，UI 的焦点和维护借用由 UI 自身的析构注销协议处理。

Object 与 Execution 的适配代码现在同属 `object` target；旧 `object_execution` 和
`object_signal_sender` target 已移除。Object 不依赖 Engine、UI、Scene 或 Editor。
