#include <lux/engine/render/device/NativeExecution.hpp>
#include <new>
#include <system_error>

namespace lux::render::detail
{
    NativeExecution::NativeExecution(cxx::execution::ManualContextConfig config) noexcept
        : context_(config, {this, notify}), producer_(context_)
    {
    }

    RenderResult<std::unique_ptr<NativeExecution>>
    NativeExecution::create(cxx::execution::ManualContextConfig config) noexcept
    {
        auto host = std::unique_ptr<NativeExecution>(new NativeExecution(config));
        try
        {
            host->worker_ = std::thread([owner = host.get()]() noexcept { owner->run(); });
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::system_error& error)
        {
            return cxx::unexpected(RenderError{kNativeThreadStart, {static_cast<std::uint64_t>(error.code().value())}});
        }
        return host;
    }

    NativeExecution::~NativeExecution() noexcept
    {
        closeAndJoin();
    }

    void NativeExecution::notify(void* value) noexcept
    {
        auto& self = *static_cast<NativeExecution*>(value);
        self.epoch_.fetch_add(1, std::memory_order_release);
        self.epoch_.notify_one();
    }

    NativeExecution::Scheduler NativeExecution::scheduler() noexcept
    {
        return producer_.get_scheduler(cxx::execution::ScheduleMode::delivery);
    }

    void NativeExecution::run() noexcept
    {
        for (;;)
        {
            const auto observed = epoch_.load(std::memory_order_acquire);
            if (context_.run_ready(128) != 0)
            {
                continue;
            }
            if (context_.is_closed())
            {
                if (context_.is_idle())
                {
                    return;
                }
                // Close may wake us before its notifier/publisher tail returns. That final
                // tail emits no second notification, so do not park during this short drain.
                std::this_thread::yield();
                continue;
            }
            epoch_.wait(observed, std::memory_order_acquire);
        }
    }

    void NativeExecution::closeAndJoin() noexcept
    {
        if (worker_.get_id() == std::this_thread::get_id())
        {
            std::terminate();
        }
        (void)context_.close();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
} // namespace lux::render::detail
