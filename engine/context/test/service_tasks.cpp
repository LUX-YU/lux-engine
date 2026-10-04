#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <optional>
#include <semaphore>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::services;
    struct Trace final
    {
        int destroyed{};
    };
    class Receiver final : public object::LuxObject
    {
    public:
        Receiver(object::ObjectDispatcherRef dispatcher, Trace& trace, process::ExecutionRuntime& execution)
            : LuxObject(std::move(dispatcher)), trace_(trace), execution_(execution)
        {
        }
        ~Receiver() override
        {
            assert(isOnAffinityThread());
            (void)execution_.wakeEpoch(); // The declared, borrowed foundation still exists at actual reclamation.
            ++trace_.destroyed;
        }
        void accept(int value) noexcept
        {
            result_ = value;
        }
        [[nodiscard]] const std::optional<int>& result() const noexcept
        {
            return result_;
        }
        void acknowledge() noexcept
        {
            result_.reset();
        }

    private:
        Trace& trace_;
        process::ExecutionRuntime& execution_;
        std::optional<int> result_;
    };
    constexpr std::array contracts{ServiceContract::forType<Receiver, Receiver>(ServiceNameView{"task.receiver"})};
    constexpr std::array dependencies{
        ServiceDependency{ServiceNameView{"task.trace"}, 1, cxx::typeToken<Trace>(), EDependencyKind::BORROWED},
        ServiceDependency{
            ServiceNameView{"task.execution"},
            1,
            cxx::typeToken<process::ExecutionRuntime>(),
            EDependencyKind::BORROWED
        }
    };
    constexpr auto factory = [](ServiceResolver& resolver,
                                const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<Receiver>>
    {
        auto trace = resolver.require<Trace>(0);
        if (!trace)
        {
            return cxx::unexpected(std::move(trace.error()));
        }
        auto execution = resolver.require<process::ExecutionRuntime>(1);
        if (!execution)
        {
            return cxx::unexpected(std::move(execution.error()));
        }
        return std::make_unique<Receiver>(resolver.dispatcher(), trace->get(), execution->get());
    };
    constexpr auto definition =
        ServiceDescriptor::forType<Receiver, factory>(ServiceNameView{"task.default"}, contracts, dependencies);
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
    template <class Fn> void until(Fn ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    void completion(bool transport)
    {
        auto execution = take(process::ExecutionRuntime::create({1, 16, 16, {16}}));
        auto messages = take(object::ObjectMessageQueue::create(16));
        Trace trace;
        ServiceRegistry services(messages.dispatcherRef());
        assert(services.publish({ServiceEntry::bind<definition>(object::CodeLease::builtin())}));
        auto scope = take(services.createScope());
        assert(scope.provide(ServiceNameView{"task.trace"}, trace));
        assert(scope.provide(ServiceNameView{"task.execution"}, execution));
        auto receiver = take(services.get<Receiver>(scope));
        std::weak_ptr<Receiver> weak = receiver;
        bool received{};
        auto make = [&](process::TaskReporter) noexcept
        {
            return stdexec::then(
                stdexec::schedule(execution.cpu()),
                []() noexcept -> cxx::expected<int, int> { return 42; }
            );
        };
        auto completed = [&, owner = std::move(receiver)](process::TTaskResult<int, int>&& result) noexcept
        {
            assert(result && *result == 42 && owner->isOnAffinityThread());
            received = true; // Only settle owned result; no UI or author mutation in the transport case.
        };
        if (transport)
        {
            process::TaskScope tasks(execution);
            const auto id = take(tasks.submit({"fixture.transport"}, make, std::move(completed)));
            assert(scope.release());
            until([&] { return execution.taskInfo(id)->finished.has_value(); });
            assert(!received && !weak.expired() && !scope.drained());
            assert(tasks.join()); // Existing collection boundary clears its accepted completion owner.
        }
        else
        {
            auto task = take(execution.submit({"fixture.business"}, make, std::move(completed)));
            assert(scope.release());
            until([&] { return execution.taskInfo(task.id())->finished.has_value(); });
            take(execution.collectCompletions());
            assert(!received && !weak.expired() && !scope.drained());
            assert(messages.collectRetired() == 0);
            take(execution.dispatchTaskEvents());
        }
        assert(received && weak.expired() && trace.destroyed == 0 && !scope.drained());
        assert(messages.collectRetired() == 1 && trace.destroyed == 1 && scope.drained());
        execution.requestStop();
        assert(execution.join());
    }
    void cancellation()
    {
        auto execution = take(process::ExecutionRuntime::create({1, 16, 16, {16}}));
        auto messages = take(object::ObjectMessageQueue::create(16));
        Trace trace;
        ServiceRegistry services(messages.dispatcherRef());
        assert(services.publish({ServiceEntry::bind<definition>(object::CodeLease::builtin())}));
        auto scope = take(services.createScope());
        assert(scope.provide(ServiceNameView{"task.trace"}, trace));
        assert(scope.provide(ServiceNameView{"task.execution"}, execution));
        auto receiver = take(services.get<Receiver>(scope));
        std::binary_semaphore entered{0}, release{0};
        auto blocker = take(execution.submit(
            {"fixture.block"},
            [&](process::TaskReporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(execution.cpu()),
                    [&]() noexcept -> cxx::expected<int, int>
                    {
                        entered.release();
                        release.acquire();
                        return 1;
                    }
                );
            },
            [](process::TTaskResult<int, int>&&) noexcept {}
        ));
        entered.acquire();
        bool cancelled{};
        auto task = take(execution.submit(
            {"fixture.cancel"},
            [&](process::TaskReporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(execution.cpu()),
                    []() noexcept -> cxx::expected<int, int>
                    {
                        assert(false);
                        return 0; // Cancellation must occur before this queued body.
                    }
                );
            },
            [&, owner = std::move(receiver)](process::TTaskResult<int, int>&& result) noexcept
            {
                assert(!result && result.error().isCancelled() && owner->isOnAffinityThread());
                cancelled = true;
            }
        ));
        task.requestStop();
        assert(scope.release() && !scope.drained());
        release.release();
        until(
            [&]
            {
                take(execution.collectCompletions());
                take(execution.dispatchTaskEvents());
                return cancelled;
            }
        );
        assert(trace.destroyed == 0);
        assert(messages.collectRetired() == 1 && scope.drained());
        execution.requestStop();
        assert(execution.join());
    }
    void retainedResult()
    {
        auto execution = take(process::ExecutionRuntime::create({1, 16, 16, {16}}));
        auto messages = take(object::ObjectMessageQueue::create(16));
        Trace trace;
        ServiceRegistry services(messages.dispatcherRef());
        auto descriptor = definition;
        descriptor.retention = EServiceRetention::SCOPED;
        assert(services.publish({ServiceEntry::create(object::CodeLease::builtin(), descriptor)}));
        auto scope = take(services.createScope());
        assert(scope.provide(ServiceNameView{"task.trace"}, trace));
        assert(scope.provide(ServiceNameView{"task.execution"}, execution));
        auto receiver = take(services.get<Receiver>(scope));
        std::weak_ptr<Receiver> weak = receiver;
        auto task = take(execution.submit(
            {"fixture.retained"},
            [&](process::TaskReporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(execution.cpu()),
                    []() noexcept -> cxx::expected<int, int> { return 73; }
                );
            },
            [owner = std::move(receiver)](process::TTaskResult<int, int>&& result) noexcept
            {
                assert(result);
                owner->accept(*result);
            }
        ));
        until([&] { return execution.taskInfo(task.id())->finished.has_value(); });
        take(execution.collectCompletions());
        take(execution.dispatchTaskEvents());
        assert(messages.collectRetired() == 0 && !weak.expired() && trace.destroyed == 0);
        // No UI or operation retains this result now; the scope retains the one allocation until review.
        auto reopened = take(services.get<Receiver>(scope));
        assert(reopened->result() == 73 && !weak.owner_before(reopened) && !reopened.owner_before(weak));
        assert(scope.beginClose());
        reopened->acknowledge();
        assert(scope.release() && !scope.drained());
        reopened.reset();
        assert(messages.collectRetired() == 1 && scope.drained());
        execution.requestStop();
        assert(execution.join());
    }
} // namespace
int main()
{
    completion(false);
    completion(true);
    cancellation();
    retainedResult();
    std::cout << "PASS real Process: accepted business completion, TaskScope collection, queued cancellation, owner "
                 "retirement\n";
}
