#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <array>
#include <cassert>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>
#include <vector>
#include <cstdlib>

using namespace lux::object;
namespace
{
    class Sender : public lux::object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        TSignal<int> changed{*this};
        TSignal<> activated{*this};
        TSignal<std::unique_ptr<int>> owned{*this};
        using LuxObject::emit;
    };
    class Receiver final : public lux::object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        int received{};
        void accept(const int& value) noexcept
        {
            received += value;
        }
    };
    void fixedBatch()
    {
        auto& messages = ObjectRuntime::instance();
        const auto before = messages.statistics();
        const auto capacity = before.capacity_per_batch;
        std::vector<std::size_t> values(capacity + 2);
        std::size_t count{};
        for (std::size_t value = 0; value < capacity; ++value)
            assert(detail::post(detail::makeMessage([&, value]() noexcept {
                values[count++] = value;
                if (value == 0)
                    assert(detail::post(detail::makeMessage([&]() noexcept {
                        values[count++] = capacity;
                    })) == detail::EPostStatus::POSTED);
            })) == detail::EPostStatus::POSTED);
        auto retained = detail::makeMessage([&]() noexcept { values[count++] = capacity + 1; });
        assert(detail::post(std::move(retained)) == detail::EPostStatus::FULL && retained);
        assert(messages.dispatchPending() == capacity && count == capacity);
        for (std::size_t i = 0; i < capacity; ++i)
            assert(values[i] == i);
        assert(detail::post(std::move(retained)) == detail::EPostStatus::POSTED && !retained);
        assert(messages.dispatchPending() == 2 && values[capacity] == capacity && values.back() == capacity + 1);
        const auto stats = messages.statistics();
        assert(stats.pending == 0 && stats.posted - before.posted == capacity + 2);
        assert(stats.inline_posted - before.inline_posted == capacity + 2 && stats.full - before.full == 1);
    }

    void closeDuringDelivery()
    {
        auto& messages = ObjectRuntime::instance();
        Sender sender;
        auto receiver = std::make_unique<Receiver>();
        unsigned calls{};
        auto first = LuxObject::connect(&sender, &Sender::activated, [&]() noexcept {
            receiver.reset();
        });
        auto second = LuxObject::connect(&sender, &Sender::activated, receiver.get(), [&]() noexcept {
            ++calls;
        }, EDelivery::QUEUED);
        assert(first && second);
        // Queue before revocation; cancelling the endpoint must suppress this accepted callback.
        auto pending = LuxObject::connect(&sender, &Sender::changed, receiver.get(), &Receiver::accept, EDelivery::QUEUED);
        assert(pending && sender.emit(sender.changed, 3).queued == 1);
        assert(sender.emit(sender.activated).direct == 1 && !second->connected() && !pending->connected());
        assert(messages.dispatchPending() == 1 && calls == 0);
    }

    void partialBroadcast()
    {
        auto& messages = ObjectRuntime::instance();
        const auto capacity = messages.statistics().capacity_per_batch;
        Sender sender;
        Receiver first, second;
        auto a = LuxObject::connect(&sender, &Sender::changed, &first, &Receiver::accept, EDelivery::QUEUED);
        auto b = LuxObject::connect(&sender, &Sender::changed, &second, &Receiver::accept, EDelivery::QUEUED);
        assert(a && b);
        for (std::size_t i = 0; i < capacity - 3; ++i)
            assert(detail::post(detail::makeMessage([]() noexcept {})) == detail::EPostStatus::POSTED);
        assert(sender.emit(sender.changed, 1).queued == 2);
        const auto partial = sender.emit(sender.changed, 2);
        assert(partial.queued == 1 && partial.full == 1 && !partial.complete());
        assert(messages.dispatchPending() == capacity && first.received == 3 && second.received == 1);
        // Foreign disconnect is not a business message and remains reliable when the queue is full.
        for (std::size_t i = 0; i < capacity; ++i)
            assert(detail::post(detail::makeMessage([]() noexcept {})) == detail::EPostStatus::POSTED);
        auto handle = std::move(*b);
        std::thread foreign([&] { handle.disconnect(); });
        foreign.join();
        assert(!handle.connected());
        assert(messages.dispatchPending() == capacity);
        assert(sender.emit(sender.changed, 4).queued == 1);
        assert(messages.dispatchPending() == 1 && first.received == 7 && second.received == 1);
    }

    void concurrentProducers()
    {
        auto& messages = ObjectRuntime::instance();
        const auto before = messages.statistics();
        std::atomic<unsigned> remaining{4};
        std::array<unsigned, 4> expected{};
        std::array<std::thread, 4> producers;
        for (unsigned producer{}; producer < 4; ++producer)
            producers[producer] = std::thread([&, producer] {
                assert(!messages.isCurrent());
                for (unsigned value{}; value < 300; ++value)
                {
                    auto message = detail::makeMessage([&, producer, value]() noexcept {
                        assert(messages.isCurrent() && expected[producer]++ == value);
                    });
                    while (detail::post(std::move(message)) == detail::EPostStatus::FULL)
                    {
                        assert(message);
                        std::this_thread::yield();
                    }
                }
                --remaining;
            });
        while (remaining.load() || messages.statistics().pending)
        {
            static_cast<void>(messages.dispatchPending());
            std::this_thread::yield();
        }
        for (auto& producer : producers)
            producer.join();
        for (auto count : expected)
            assert(count == 300);
        const auto stats = messages.statistics();
        assert(stats.posted - before.posted == 1200 && stats.inline_posted - before.inline_posted == 1200);
        assert(stats.high_water <= 2 * stats.capacity_per_batch);
    }

    void connectionLifetime()
    {
        auto& messages = ObjectRuntime::instance();
        struct Derived final : Sender
        {
            using Sender::Sender;
        };
        Derived sender{};
        Receiver receiver{};
        {
            auto connection = LuxObject::connect(&sender, &Sender::changed, &receiver, &Receiver::accept);
            assert(connection && sender.emit(sender.changed, 3).direct == 1 && receiver.received == 3);
            auto moved = std::move(*connection);
            assert(!connection->connected() && moved.connected());
        }
        assert(sender.emit(sender.changed, 2).direct == 0 && receiver.received == 3);
        assert(!LuxObject::connect(&sender, &Sender::changed, static_cast<Receiver*>(nullptr), &Receiver::accept));
        auto noncopy = LuxObject::connect(&sender, &Sender::owned, [](const std::unique_ptr<int>& value) noexcept {
            assert(*value == 7);
        });
        assert(noncopy && sender.emit(sender.owned, std::make_unique<int>(7)).direct == 1);
        auto rejected = LuxObject::connect(
            &sender,
            &Sender::owned,
            &receiver,
            [](const std::unique_ptr<int>&) noexcept {},
            EDelivery::QUEUED
        );
        assert(!rejected && rejected.error() == EConnectError::PAYLOAD_NOT_QUEUEABLE);
        Connection retired;
        {
            auto temporary = std::make_unique<Receiver>();
            auto made =
                LuxObject::connect(&sender, &Sender::changed, temporary.get(), &Receiver::accept, EDelivery::QUEUED);
            assert(made);
            retired = std::move(*made);
            assert(sender.emit(sender.changed, 9).queued == 1);
        }
        assert(!retired.connected());
        // Reuse the storage while the old queued message and handle still exist.
        auto fresh = LuxObject::connect(&sender, &Sender::changed, &receiver, &Receiver::accept, EDelivery::QUEUED);
        assert(fresh && sender.emit(sender.changed, 5).queued == 1);
        retired.disconnect();
        assert(messages.dispatchPending() == 2 && receiver.received == 8);
        Connection sender_first;
        {
            Sender temporary{};
            auto made =
                LuxObject::connect(&temporary, &Sender::changed, &receiver, &Receiver::accept, EDelivery::QUEUED);
            assert(made);
            sender_first = std::move(*made);
            assert(temporary.emit(temporary.changed, 100).queued == 1);
        }
        assert(!sender_first.connected() && messages.dispatchPending() == 1 && receiver.received == 8);
    }

    void changesDuringEmit()
    {
        auto& messages = ObjectRuntime::instance();
        Sender sender{};
        Connection first, second, added;
        unsigned outer{}, later{};
        bool nested{};
        auto made = LuxObject::connect(&sender, &Sender::activated, [&]() noexcept {
            ++outer;
            second.disconnect();
            if (!nested)
            {
                nested = true;
                auto pending = LuxObject::connect(&sender, &Sender::activated, [&]() noexcept { ++later; });
                assert(pending);
                added = std::move(*pending);
                assert(sender.emit(sender.activated).direct == 1);
            }
        });
        assert(made);
        first = std::move(*made);
        auto suppressed = LuxObject::connect(&sender, &Sender::activated, []() noexcept { assert(false); });
        assert(suppressed);
        second = std::move(*suppressed);
        assert(sender.emit(sender.activated).direct == 1 && outer == 2 && later == 0);
        assert(sender.emit(sender.activated).direct == 2 && outer == 3 && later == 1);
        first.disconnect();
        added.disconnect();
        auto resource = std::make_shared<int>(1);
        std::weak_ptr<int> observed = resource;
        auto self = LuxObject::connect(&sender, &Sender::activated, [resource, &first, &observed]() noexcept {
            first.disconnect();
            assert(!observed.expired()); // Callable survives its own disconnect until invoke returns.
        });
        assert(self);
        first = std::move(*self);
        resource.reset();
        assert(sender.emit(sender.activated).direct == 1 && observed.expired());
    }

    void workerPostsToOwner()
    {
        auto& runtime = ObjectRuntime::instance();
        Receiver receiver;
        const auto id = receiver.objectId();
        std::thread worker([id]() noexcept {
            auto wrong = ObjectRuntime::instance().resolve(id);
            assert(!wrong && wrong.error() == EObjectTreeError::WRONG_THREAD);
            assert(detail::post(detail::makeMessage([id]() noexcept {
                auto target = ObjectRuntime::instance().resolve(id);
                assert(target);
                static_cast<Receiver*>(*target)->accept(11);
            })) == detail::EPostStatus::POSTED);
        });
        worker.join();
        assert(runtime.dispatchPending() == 1 && receiver.received == 11);
    }

    unsigned shutdown_calls{}, shutdown_cleanup{};
    struct ShutdownPayload final
    {
        ~ShutdownPayload()
        {
            auto late = detail::makeMessage([]() noexcept { ++shutdown_calls; });
            assert(detail::post(std::move(late)) == detail::EPostStatus::CLOSED && late);
            ++shutdown_cleanup;
            assert(shutdown_calls == 0 && shutdown_cleanup == 1);
            std::puts("Runtime shutdown: discarded without dispatch; CLOSED retained");
            std::fflush(stdout);
        }
    };

}
int main(int argc, char**)
{
    if (argc > 1)
    {
        auto& runtime = ObjectRuntime::instance();
        assert(runtime.isCurrent());
        auto payload = std::make_shared<ShutdownPayload>();
        assert(detail::post(detail::makeMessage([payload = std::move(payload)]() noexcept {
            ++shutdown_calls;
        })) == detail::EPostStatus::POSTED);
        return 0;
    }
    (void)ObjectRuntime::instance();
    fixedBatch();
    closeDuringDelivery();
    partialBroadcast();
    concurrentProducers();
    connectionLifetime();
    changesDuringEmit();
    workerPostsToOwner();
}
