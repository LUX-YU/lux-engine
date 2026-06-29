#pragma once
/**
 * @file EngineExecutorSenders.hpp
 * @brief opt-in 适配层:把 `EngineExecutor` 的两个调度器(CPU 池 / 主线程汇合点)+ 其
 *        `async_scope` 暴露给需要在它们之上组装 stdexec sender 的消费方。
 *
 * 与 `RenderRequestSender.hpp` 同构 —— **本头包含 stdexec**,故包含它的 TU 需自带
 * `STDEXEC::stdexec` 链接 + MSVC `/permissive- /Zc:__cplusplus /Zc:preprocessor`。门面
 * `EngineExecutor.hpp` 仍 stdexec-free:它只暴露三个 `void*` 句柄,本头把它们强转回真实
 * 的 stdexec 子对象类型(地址来自活着的 `Impl` 子对象,转回的正是原类型 —— 行为完全确定,
 * 无 reinterpret_cast、无别名危害)。
 *
 * 典型用法:
 * @code
 *   lux::exec::spawn(exec,
 *       stdexec::schedule(lux::exec::cpuScheduler(exec))
 *     | stdexec::then(cpuWork)
 *     | stdexec::continues_on(lux::exec::mainScheduler(exec))
 *     | stdexec::then(mainWork));     // 末端须 set_value()(void),spawn 的 receiver 只收 set_value()
 * @endcode
 */

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/detail/MainScheduler.hpp>   // MainQueue / MainScheduler(opt-in stdexec)

#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>
#include <exec/async_scope.hpp>

#include <utility>

namespace lux::exec
{
    /// CPU 后台池的 conforming scheduler。`schedule(cpuScheduler(e))` 把工作落到池线程
    /// (帧无关,远离主线程)。返回类型用 auto 推导,免去拼写 stdexec 内嵌类型名。
    [[nodiscard]] inline auto cpuScheduler(EngineExecutor& e) noexcept
    {
        return static_cast<::exec::static_thread_pool*>(e.cpuPoolHandle())->get_scheduler();
    }

    /// 主线程汇合点 scheduler。其任务在 `EngineExecutor::drainMain()`(app 主循环每帧、
    /// 帧-OPEN)被抽干。`continues_on(mainScheduler(e))` 把下游 continuation 拉回主线程。
    [[nodiscard]] inline MainScheduler mainScheduler(EngineExecutor& e) noexcept
    {
        return MainScheduler{*static_cast<MainQueue*>(e.mainQueueHandle())};
    }

    /// 在 EngineExecutor 自己的 async_scope 上起一条 sender(fire-and-forget)。
    /// @warning @p s 末端必须以 `set_value()`(void)收尾 —— async_scope 的 spawn-receiver
    ///          只接受 `set_value()`;用 `then` 末端收口、用 `upon_error`/`upon_stopped`
    ///          把 error/stopped 通道也塌缩成 void。须在主线程、executor 存活且未 shutdown 时调。
    template <class Sender>
    void spawn(EngineExecutor& e, Sender&& s)
    {
        static_cast<::exec::async_scope*>(e.asyncScopeHandle())->spawn(std::forward<Sender>(s));
    }

} // namespace lux::exec
