# 修复前真实执行器测试草图（本包未运行）

复用当前 `editor/tools/material/preview/test/compilation.cpp` 中的真实 `materialSource`、`take`、作者会话和执行器初始化。以下是待接入的顺序，不是已编译程序。

## Material

```cpp
auto op = take(MaterialCompileOperation::start(execution, take(author.capture()), {}, 1, 1));
const auto task = op->task();
// 有界轮询实际 execution.taskInfo(task)->finished；不要在此派发 task events。
wait_until_worker_finished_without_dispatch(execution, task);
assert(!op->ready());
{
    auto duplicate = *op; // 当前版本合法；修复后必须是编译负例。
    assert(duplicate.id() == op->id());
}
assert(execution.collectCompletions());
assert(execution.dispatchTaskEvents());
// 记录 op 仍存在、worker 已完成、原 op ready。目标契约是原 owner 不被副本扰动。
// 若仍未 ready，打印并返回非零；不要重新 start 或修改 completed。
```

## Flow

```cpp
FlowCompilationService service(execution, 1);
auto id = take(service.start(take(author.capture()), {}, {}, {"missing-R07-linker.exe"}));
const auto task = take(service.operation(id)).get().task();
wait_until_worker_finished_without_dispatch(execution, task);
assert(!take(service.operation(id)).get().ready());
{
    auto duplicate = take(service.operation(id)).get(); // const 借用被 auto 复制。
    assert(duplicate.id() == id);
}
assert(execution.collectCompletions());
assert(execution.dispatchTaskEvents());
// 记录 service 内原记录仍存在但是否 ready，ack 是否 BUSY，新准入是否 CAPACITY。
```

`wait_until_worker_finished_without_dispatch` 是测试同步占位符，需使用现有 TaskInfo 有界轮询实现；本包没有引入生产 API。不要无限等待。当前默认诊断历史可能保留任务信息，因此不要只用 taskInfos().empty() 判断活动任务。

修复后的常规测试改为移动 Material 的 unique_ptr 与借用 Flow 的 const 引用，不再编译上述非法复制。修复前消费者源码和运行记录冻结在 before/；负向编译用例只检查指定禁止操作，不因缺包或缺头误判。
