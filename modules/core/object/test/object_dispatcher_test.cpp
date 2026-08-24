#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "ObjectTestSignals.hpp"
#include "ObjectDiagnostics.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
    using lux::object::test::fixture::IntSender;

    class Receiver final : public lux::object::Object<Receiver>
    {
      public:
        explicit Receiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        void receive(const int &value) noexcept
        {
            observed = value;
        }
        int observed{0};
    };

    struct RetainedCallable final
    {
        std::shared_ptr<int> lifetime;

        void operator()(const int &) noexcept
        {
        }
    };

    struct Ping final
    {
        int value{0};
    };

    struct SmallMessageProbe final
    {
        static inline std::atomic_size_t allocations{0};
        bool *invoked{nullptr};

        static void *operator new(std::size_t size)
        {
            allocations.fetch_add(1, std::memory_order_relaxed);
            return ::operator new(size);
        }
        static void operator delete(void *value) noexcept
        {
            ::operator delete(value);
        }
        void operator()() noexcept
        {
            *invoked = true;
        }
    };

    struct LargeMessageProbe final
    {
        static inline std::atomic_size_t allocations{0};
        std::array<std::byte, 128> padding{};
        bool *invoked{nullptr};

        static void *operator new(std::size_t size)
        {
            allocations.fetch_add(1, std::memory_order_relaxed);
            return ::operator new(size);
        }
        static void operator delete(void *value) noexcept
        {
            ::operator delete(value);
        }
        void operator()() noexcept
        {
            *invoked = true;
        }
    };

    struct CloseReentryProbe final
    {
        CloseReentryProbe(lux::object::ObjectDispatcherRef value,
                          std::shared_ptr<std::atomic_bool> armed_value,
                          std::atomic<lux::object::detail::EPostStatus> *status_value) noexcept
            : dispatcher(std::move(value)), armed(std::move(armed_value)), status(status_value)
        {
        }

        CloseReentryProbe(CloseReentryProbe &&other) noexcept
            : dispatcher(std::move(other.dispatcher)), armed(std::move(other.armed)),
              status(std::exchange(other.status, nullptr))
        {
        }

        CloseReentryProbe(const CloseReentryProbe &) = delete;
        CloseReentryProbe &operator=(const CloseReentryProbe &) = delete;

        ~CloseReentryProbe()
        {
            if (!status || !armed->exchange(false, std::memory_order_acq_rel))
                return;
            auto nested = lux::object::detail::makeMessage([]() noexcept {});
            status->store(lux::object::detail::post(dispatcher, std::move(nested)),
                          std::memory_order_release);
        }

        void operator()() noexcept
        {
        }

        lux::object::ObjectDispatcherRef dispatcher;
        std::shared_ptr<std::atomic_bool> armed;
        std::atomic<lux::object::detail::EPostStatus> *status{nullptr};
    };

    class EventReceiver final : public lux::object::Object<EventReceiver>
    {
      public:
        explicit EventReceiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        int observed{0};

      protected:
        void event(lux::object::EventView &view) noexcept override
        {
            if (auto *ping = view.getIf<Ping>())
            {
                observed = ping->value;
                view.accept();
            }
        }
    };

    void runQueuedScenario(bool disconnect, bool destroy_receiver)
    {
        lux::object::ObjectMessageQueue sender_queue;
        IntSender sender{sender_queue.dispatcherRef()};
        std::mutex mutex;
        std::condition_variable condition;
        Receiver *receiver_pointer = nullptr;
        bool ready = false;
        bool drain = false;
        int observed = -1;
        std::size_t dispatched = 0;

        std::thread receiver_thread([&] {
            lux::object::ObjectMessageQueue queue;
            auto receiver = std::make_unique<Receiver>(queue.dispatcherRef());
            {
                std::scoped_lock lock{mutex};
                receiver_pointer = receiver.get();
                ready = true;
            }
            condition.notify_all();
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [&] { return drain; });
            }
            if (destroy_receiver)
                receiver.reset();
            dispatched = queue.dispatchPending();
            observed = receiver ? receiver->observed : 0;
        });

        {
            std::unique_lock lock{mutex};
            condition.wait(lock, [&] { return ready; });
        }
        auto connection =
            sender.observe<IntSender::changed, &Receiver::receive, lux::object::EDelivery::AUTO>(
                *receiver_pointer);
        assert(connection);
        sender.publish(7);
        if (disconnect)
        {
            std::thread disconnect_thread(
                [connection_copy = *connection]() mutable { connection_copy.disconnect(); });
            disconnect_thread.join();
            assert(!connection->connected());
            assert(sender_queue.dispatchPending() == 1);
        }
        {
            std::scoped_lock lock{mutex};
            drain = true;
        }
        condition.notify_all();
        receiver_thread.join();

        assert(dispatched == 1);
        if (disconnect || destroy_receiver)
        {
            assert(observed == 0);
            assert(!connection->connected());
        }
        else
        {
            assert(observed == 7);
        }
    }
} // namespace

int main()
{
    bool small_invoked = false;
    auto small_message = lux::object::detail::makeMessage(SmallMessageProbe{&small_invoked});
    assert(SmallMessageProbe::allocations.load(std::memory_order_relaxed) == 0);
    lux::object::ObjectMessageQueue inline_queue;
    assert(lux::object::detail::post(inline_queue.dispatcherRef(), std::move(small_message)) ==
           lux::object::detail::EPostStatus::POSTED);
    assert(inline_queue.dispatchPending() == 1);
    assert(small_invoked);

    bool large_invoked = false;
    LargeMessageProbe large_probe;
    large_probe.invoked = &large_invoked;
    auto large_message = lux::object::detail::makeMessage(std::move(large_probe));
    assert(LargeMessageProbe::allocations.load(std::memory_order_relaxed) == 1);
    assert(lux::object::detail::post(inline_queue.dispatcherRef(), std::move(large_message)) ==
           lux::object::detail::EPostStatus::POSTED);
    assert(inline_queue.dispatchPending() == 1);
    assert(large_invoked);

    runQueuedScenario(false, false);
    runQueuedScenario(true, false);
    runQueuedScenario(false, true);

    {
        IntSender sender_without_dispatcher;
        lux::object::ObjectMessageQueue receiver_queue;
        Receiver receiver{receiver_queue.dispatcherRef()};
        auto connection =
            sender_without_dispatcher
                .observe<IntSender::changed, &Receiver::receive, lux::object::EDelivery::QUEUED>(
                    receiver);
        assert(!connection);
        assert(connection.error() == lux::object::EObserveError::SENDER_HAS_NO_DISPATCHER);
    }

    {
        lux::object::ObjectMessageQueue sender_queue;
        IntSender sender{sender_queue.dispatcherRef()};
        Receiver receiver{lux::object::ObjectDispatcherRef{}};
        auto observed = sender.observe<IntSender::changed, &Receiver::receive,
                                       lux::object::EDelivery::DIRECT>(receiver);
        assert(observed);
        auto relation = std::move(*observed);
        auto relation_copy = relation;
        assert(lux::object::detail::ObjectDiagnosticsAccess::ownedConnectionCount(sender) == 1);
        assert(lux::object::detail::ObjectDiagnosticsAccess::incomingConnectionCount(receiver) == 1);

        std::thread disconnect_thread([&] { relation.disconnect(); });
        disconnect_thread.join();
        assert(!relation.connected());
        assert(!relation_copy.connected());
        relation_copy.disconnect();
        assert(lux::object::detail::ObjectDiagnosticsAccess::ownedConnectionCount(sender) == 1);
        assert(lux::object::detail::ObjectDiagnosticsAccess::incomingConnectionCount(receiver) == 1);
        assert(sender_queue.dispatchPending() == 1);
        assert(lux::object::detail::ObjectDiagnosticsAccess::ownedConnectionCount(sender) == 0);
        assert(lux::object::detail::ObjectDiagnosticsAccess::incomingConnectionCount(receiver) == 0);
    }

    {
        lux::object::ObjectMessageQueue sender_queue;
        IntSender sender{sender_queue.dispatcherRef()};
        auto lifetime = std::make_shared<int>(17);
        std::weak_ptr<int> weak_lifetime = lifetime;
        auto scoped =
            sender.observeScoped<IntSender::changed>(RetainedCallable{std::move(lifetime)});
        assert(scoped);
        auto connection = scoped.release();
        auto connection_copy = connection;

        std::thread disconnect_thread([&] { connection.disconnect(); });
        disconnect_thread.join();
        assert(!connection.connected());
        assert(!connection_copy.connected());
        connection_copy.disconnect();
        assert(!weak_lifetime.expired());
        assert(sender_queue.dispatchPending() == 1);
        assert(weak_lifetime.expired());
    }

    {
        lux::object::ObjectMessageQueue queue;
        auto armed = std::make_shared<std::atomic_bool>(true);
        std::atomic status{lux::object::detail::EPostStatus::POSTED};
        auto message = lux::object::detail::makeMessage(
            CloseReentryProbe{queue.dispatcherRef(), std::move(armed), &status});
        assert(lux::object::detail::post(queue.dispatcherRef(), std::move(message)) ==
               lux::object::detail::EPostStatus::POSTED);
        queue.close();
        assert(status.load(std::memory_order_acquire) == lux::object::detail::EPostStatus::CLOSED);
    }

    {
        lux::object::ObjectMessageQueue queue;
        lux::object::ObjectWeakRef expired_target;
        {
            EventReceiver receiver{queue.dispatcherRef()};
            expired_target = receiver.weakRef();
            assert(lux::object::postEvent(expired_target, Ping{23}) ==
                   lux::object::EEventPostStatus::POSTED);
        }
        assert(expired_target.expired());
        assert(queue.dispatchPending() == 1);
    }

    {
        std::mutex mutex;
        std::condition_variable condition;
        lux::object::ObjectWeakRef target;
        bool ready = false;
        bool drain = false;
        int observed = 0;
        std::thread receiver_thread([&] {
            lux::object::ObjectMessageQueue queue;
            EventReceiver receiver{queue.dispatcherRef()};
            {
                std::scoped_lock lock{mutex};
                target = receiver.weakRef();
                ready = true;
            }
            condition.notify_all();
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [&] { return drain; });
            }
            assert(queue.dispatchPending() == 1);
            observed = receiver.observed;
        });
        {
            std::unique_lock lock{mutex};
            condition.wait(lock, [&] { return ready; });
        }
        assert(lux::object::postEvent(target, Ping{47}) == lux::object::EEventPostStatus::POSTED);
        {
            std::scoped_lock lock{mutex};
            drain = true;
        }
        condition.notify_all();
        receiver_thread.join();
        assert(observed == 47);
    }

    lux::object::ObjectDispatcherRef closed_dispatcher;
    {
        lux::object::ObjectMessageQueue queue;
        closed_dispatcher = queue.dispatcherRef();
        EventReceiver receiver{queue.dispatcherRef()};
        assert(lux::object::postEvent(receiver.weakRef(), Ping{31}) ==
               lux::object::EEventPostStatus::POSTED);
        assert(queue.dispatchPending() == 1);
        assert(receiver.observed == 31);
    }
    bool invoked = false;
    auto message = lux::object::detail::makeMessage([&] { invoked = true; });
    assert(lux::object::detail::post(closed_dispatcher, std::move(message)) ==
           lux::object::detail::EPostStatus::CLOSED);
    assert(!invoked);
}
