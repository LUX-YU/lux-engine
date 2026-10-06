# LuxObject、信号与事件

`object` 不依赖 UI、Scene 或 meta。LuxObject 提供非拥有父链、同步事件和模板信号。

ObjectRuntime 在 object 动态库中定义进程唯一实例；宿主先于 worker 首次访问它，确定 owner 线程。
所有 LuxObject 在该线程构造、访问和实际析构。默认构造取得全局代际 ObjectId；注册表只保存非拥有地址。
resolve() 只提供一次同步借用，不能跨回调或跨帧保存。对象关闭后撤销身份与接收资格，槽位复用不恢复旧 ID。

setParent()/addChild()/removeChild() 仅修改关系，不转移删除责任。addChild 可原子移动到新父对象；
重复设置同一父对象成功但不改链。线程、环、关闭与活动结构资格验证失败时原树完整。
父对象析构只解绑剩余孩子；成员、unique_ptr、shared_ptr 决定实际析构。需要派生成员的内容必须在该资源
仍有效时由实际 C++ owner 清理。UI 的类型化入口执行自己的拓扑验证。

Runtime 复用有界双批次消息与连接维护；dispatchPending() 处理固定批次，回调追加留到后批。
FULL/CLOSED 保留原语义。worker 只能投递拥有型消息、断开连接或交还已准备的回收责任。
一个 Root 或 Editor 析构不会关闭进程 Runtime，重复创建框架实例仍可使用它。

shareOnRuntime() 转移真实唯一 allocation 到共享控制块。最后引用可在 worker 释放；回收节点预先准备，
实际析构在 collectRetired() 的 owner 安全点执行，不依赖父托管或可能已满的业务队列。
Runtime 最终清理关闭准入、结清已交还的回收责任，不派发业务回调、不等待任务。
仍有外部共享 owner、活动回调或错误线程使回收无法推进时报告寿命契约错误。
CodeLease/pinCodeOwner 的宿主释放桥覆盖对象析构、状态型 deleter 清理和返回；weak 控制块可晚于 DLL 卸载。
共享准入失败不消费候选或代码 pin，deleter 移动期间仍受结构保护。

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

connect/emit 在唯一 owner 线程执行。DIRECT 同步交付；QUEUED 使用 Runtime 后批；AUTO 对同域对象同步交付。无接收方 lambda 仅 DIRECT。异线程可断开 Connection，原子取消立即阻止后续接纳，容器回收在发送方安全点进行；disconnect 不是 join，已开始回调可以完成。

每信号惰性建立 `StableSlotMap`，使用 lux-cxx 的 `SlotKey`，发送只遍历该信号。最外层派发固定可见范围；期间新增连接下次可见，断开的未开始回调跳过。接收端反向链用于关闭，不是第二份订阅表。取消维护不使用可能 FULL 的业务消息；取消记录由 Runtime 安全点和 connect/emit/关闭收尾。

`SignalDelivery` 分别报告 direct、queued、full、closed。排队载荷由消息工厂固定，实际执行时重查取消及接收端存活。队列不会读取已经销毁的对象。消息准备中的内存不足或载荷复制异常会终止进程。

同步 `EventView` 支持过滤和父链传播，与信号广播分开。当前对象及祖先不能在回调栈中析构；对象在自己的线程销毁，析构不泵消息。关闭业务和结束编辑须先于对象析构。没有公开 `ObjectWeakRef`，UI 的焦点和维护借用由 UI 自身的析构注销协议处理。

测试 `object.tree`、`object.queue` 保留路由和信号行为；`object.ownership` 验证非拥有混合树、
拒绝不消费、清理期重入、安全点和真实 DLL 析构尾部。独立 object-ownership SDK 消费者使用安装的
公共头和库重跑这些行为。测试入口的存在不代表已执行，实际结果以阶段验收记录为准。
