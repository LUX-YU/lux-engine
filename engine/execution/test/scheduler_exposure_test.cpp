// ============================================================================
//  scheduler_exposure_test — Phase C1. Self-checking exe (exit 0 on success).
//
//  Validates `EngineExecutorSenders.hpp`: that cpuScheduler / mainScheduler /
//  spawn correctly expose the EngineExecutor's CPU pool + main-thread join point
//  through the opaque void* handles. The COMPILE is the primary conformance check
//  (if the schedulers were non-conforming, schedule/continues_on/spawn would fail
//  to instantiate). The RUN asserts the threading contract end-to-end:
//    schedule(cpu) | then(record pool tid) | continues_on(main) | then(record main tid)
//  the CPU step must run OFF the main thread; the main step must run ON the thread
//  that calls drainMain() (the main thread). No GPU / render / asset IO involved.
// ============================================================================

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/EngineExecutorSenders.hpp>

#include <lux/engine/asset/AssetManager.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <thread>

namespace ex = stdexec;

int main()
{
    int failures = 0;
    auto check = [&](bool cond, const char* what)
    {
        if (!cond) { std::printf("[FAIL] %s\n", what); ++failures; }
        else       { std::printf("[ ok ] %s\n", what); }
    };

    lux::asset::AssetManager   mgr;          // default-constructed; no VFS, never touched here
    lux::exec::EngineExecutor  exec(mgr, /*cpu_threads=*/2);

    const std::thread::id main_tid = std::this_thread::get_id();
    std::thread::id  pool_tid{};
    std::thread::id  main_step_tid{};
    std::atomic<bool> done{false};

    // CPU work off-main, then hop back to the main join point and record where each ran.
    // Ends by collapsing set_stopped -> set_value() so async_scope::spawn (set_value-only
    // receiver) accepts it — mirrors requestLoad's terminal upon_stopped.
    lux::exec::spawn(exec,
        ex::schedule(lux::exec::cpuScheduler(exec))
      | ex::then([&]() noexcept { pool_tid = std::this_thread::get_id(); })
      | ex::continues_on(lux::exec::mainScheduler(exec))
      | ex::then([&]() noexcept
        {
            main_step_tid = std::this_thread::get_id();
            done.store(true, std::memory_order_release);
        })
      | ex::upon_stopped([]() noexcept {}));

    // Pump the main queue (as the app loop would) until the main continuation runs.
    for (int i = 0; i < 1'000'000 && !done.load(std::memory_order_acquire); ++i)
    {
        exec.drainMain();
        std::this_thread::yield();
    }

    check(done.load(std::memory_order_acquire), "pipeline completed (main step ran via drainMain)");
    check(pool_tid != std::thread::id{},        "cpu step actually ran");
    check(pool_tid != main_tid,                 "cpu step ran OFF the main thread (on the pool)");
    check(main_step_tid == main_tid,            "main step ran ON the main thread (drainMain caller)");

    exec.shutdown();   // idempotent; scope already empty

    std::printf(failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
