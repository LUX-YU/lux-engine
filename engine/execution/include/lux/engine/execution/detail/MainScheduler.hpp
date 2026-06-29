#pragma once
/**
 * @file detail/MainScheduler.hpp
 * @brief 主线程汇合点原语:线程安全任务队列 `MainQueue` + 其上的 conforming stdexec
 *        scheduler `MainScheduler`。
 *
 * **opt-in stdexec 头** —— 它包含 `<stdexec/execution.hpp>`,故任何包含它的 TU 都被
 * stdexec 头 + MSVC `/permissive- /Zc:__cplusplus /Zc:preprocessor` 传染。门面
 * `EngineExecutor.hpp` 不包含它,保持 stdexec-free;只有需要在 EngineExecutor 的调度器上
 * 组装 sender 的消费方(经 `EngineExecutorSenders.hpp`)与本模块 .cpp 才包含它。
 *
 * 从 `EngineExecutor.cpp` 的匿名命名空间迁出(逐字),改放 `namespace lux::exec`(**非**
 * 匿名)—— 因为 `EngineExecutorSenders.hpp` 把 `EngineExecutor::mainQueueHandle()` 返回的
 * `void*` 强转回 `MainQueue*`,二者必须是**同一**定义(否则静默 ODR 违例)。
 */

#include <lux/cxx/compile_time/move_only_function.hpp>

#include <stdexec/execution.hpp>

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace lux::exec
{
    namespace ex = ::stdexec;

    // ── 主线程汇合点:线程安全任务队列 + pump。任意线程 enqueue,主线程 drainMain 跑掉。
    class MainQueue
    {
    public:
        using Task = lux::cxx::move_only_function<void()>;

        void enqueue(Task t)
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(t));
        }

        std::size_t pump(std::size_t max_n)
        {
            std::size_t ran = 0;
            while (ran < max_n)
            {
                Task t;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    if (q_.empty()) break;
                    t = std::move(q_.front());
                    q_.pop_front();
                }
                if (t) t();          // 锁外执行 —— task 内可再 enqueue,不自死锁
                ++ran;
            }
            return ran;
        }

        [[nodiscard]] bool empty() const
        {
            std::lock_guard<std::mutex> lk(m_);
            return q_.empty();
        }

    private:
        mutable std::mutex m_;
        std::deque<Task>   q_;
    };

    // ── conforming stdexec scheduler over MainQueue。使 `continues_on(MainScheduler{q})`
    //    把下游 continuation 落到主线程 pump 点(drainMain)。
    class MainScheduler
    {
    public:
        explicit MainScheduler(MainQueue& q) noexcept : q_(&q) {}
        bool operator==(const MainScheduler&) const noexcept = default;

        struct _sender
        {
            using sender_concept = ex::sender_t;
            using completion_signatures =
                ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;

            MainQueue* q_{nullptr};

            struct _env
            {
                MainQueue* q_{nullptr};
                MainScheduler query(
                    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept
                {
                    return MainScheduler{*q_};
                }
            };
            _env get_env() const noexcept { return _env{q_}; }

            template <class Rcvr>
            struct _op
            {
                using operation_state_concept = ex::operation_state_t;
                MainQueue* q_;
                Rcvr       rcvr_;
                void start() & noexcept
                {
                    q_->enqueue([this]() { ex::set_value(std::move(rcvr_)); });
                }
            };

            template <class Rcvr>
            _op<std::decay_t<Rcvr>> connect(Rcvr&& r) const
            {
                return _op<std::decay_t<Rcvr>>{q_, std::forward<Rcvr>(r)};
            }
        };

        [[nodiscard]] _sender schedule() const noexcept { return _sender{q_}; }

    private:
        MainQueue* q_{nullptr};
    };

} // namespace lux::exec
