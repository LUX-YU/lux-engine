#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/LuxObject.hpp>

namespace lux::object::detail
{
    enum class EListenerLane : std::uint8_t
    {
        DIRECT,
        QUEUED,
        PENDING
    };

    struct ConnectionControl final
    {
        std::atomic_size_t refs{0};
        std::atomic_bool connected{true};
        std::uint64_t id{0};
        std::size_t signal_index{0};
        std::size_t position{0};
        EListenerLane lane{EListenerLane::DIRECT};
        EDelivery delivery{EDelivery::DIRECT};
        lux::cxx::intrusive_ptr<ObjectState> receiver;
        ObjectDispatcherRef receiver_dispatcher;
        ObjectInvokeThunk invoke{nullptr};
        QueuedMessageFactory queue_factory{nullptr};
        std::shared_ptr<void> context;
    };

    struct SignalBucket final
    {
        std::vector<ConnectionControl*> direct;
        std::vector<ConnectionControl*> queued;
        std::vector<ConnectionControl*> pending;
    };

    struct IncomingLink final
    {
        lux::cxx::intrusive_ptr<ObjectState> sender;
        lux::cxx::intrusive_ptr<ConnectionControl> control;
    };

    struct ObjectState final
    {
        ObjectState(
            LuxObject* value,
            ObjectDispatcherRef dispatcher_value,
            std::thread::id affinity_value
        ) noexcept;
        ~ObjectState();

        std::atomic_size_t refs{0};
        std::atomic<LuxObject*> object{nullptr};
        ObjectDispatcherRef dispatcher;
        std::thread::id affinity;

        std::vector<SignalBucket> buckets;

        using ConnectionMap = std::
            unordered_map<std::uint64_t, lux::cxx::intrusive_ptr<ConnectionControl>>;

        ConnectionMap connections;
        std::uint64_t next_connection_id{1};
        std::size_t active_notify_depth{0};
        std::atomic_bool maintenance_requested{false};
        std::mutex incoming_mutex;
        std::vector<IncomingLink> incoming;

        [[nodiscard]] lux::cxx::intrusive_ptr<ConnectionControl> install(
            const SignalRuntime& signal,
            lux::cxx::intrusive_ptr<ObjectState> receiver,
            ObjectDispatcherRef receiver_dispatcher,
            ObjectInvokeThunk invoke,
            EDelivery delivery,
            QueuedMessageFactory queue_factory,
            std::shared_ptr<void> context
        );

        void notify(const SignalRuntime& signal, const void* payload);
        void requestDisconnect(ConnectionControl* control) noexcept;
        void removeConnection(ConnectionControl* control) noexcept;
        void maintain() noexcept;
        void finishNotify() noexcept;
        void closeOwner() noexcept;

        void addIncoming(
            lux::cxx::intrusive_ptr<ObjectState> sender,
            lux::cxx::intrusive_ptr<ConnectionControl> control
        );

        void removeIncoming(
            const ObjectState* sender,
            const ConnectionControl* control
        ) noexcept;

    private:
        void
        append(SignalBucket& bucket, ConnectionControl& control, EDelivery delivery);
        void ensureSignalCapacity(std::size_t required_count);
        void removePhysical(ConnectionControl& control) noexcept;
    };
} // namespace lux::object::detail
