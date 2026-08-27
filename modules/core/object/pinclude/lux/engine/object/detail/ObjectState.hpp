#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
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
        std::size_t owner_position{0};
        std::size_t signal_index{0};
        std::size_t position{0};
        EListenerLane lane{EListenerLane::DIRECT};
        EDelivery delivery{EDelivery::DIRECT};
        lux::cxx::intrusive_ptr<ObjectState> receiver;
        ObjectInvokeThunk invoke{nullptr};
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
        ObjectState(LuxObject* value, ObjectDispatcherRef dispatcher_value, std::thread::id affinity_value) noexcept;
        ~ObjectState();

        std::atomic_size_t refs{0};
        std::atomic<LuxObject*> object{nullptr};
        ObjectDispatcherRef dispatcher;
        std::thread::id affinity;

        std::vector<SignalBucket> buckets;

        std::vector<lux::cxx::intrusive_ptr<ConnectionControl>> owned_connections;
        std::vector<ConnectionControl*> pending_removals;
        std::size_t active_notify_depth{0};
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        std::uint64_t storage_growth_count{0};
#endif
        std::mutex incoming_mutex;
        std::vector<IncomingLink> incoming;

        [[nodiscard]] lux::cxx::intrusive_ptr<ConnectionControl> install(
            const SignalDescriptor& signal,
            lux::cxx::intrusive_ptr<ObjectState> receiver,
            ObjectInvokeThunk invoke,
            EDelivery delivery,
            std::shared_ptr<void> context
        );

        void notify(const SignalDescriptor& signal, const void* payload);
        void requestDisconnect(ConnectionControl* control) noexcept;
        void removeConnection(ConnectionControl* control) noexcept;
        void maintainAfterNotify() noexcept;
        void finishNotify() noexcept;
        void closeOwner() noexcept;

        void
        addIncoming(lux::cxx::intrusive_ptr<ObjectState> sender, lux::cxx::intrusive_ptr<ConnectionControl> control);

        void removeIncoming(const ObjectState* sender, const ConnectionControl* control) noexcept;

    private:
        void append(SignalBucket& bucket, ConnectionControl& control, EDelivery delivery);
        void ensureSignalCapacity(std::size_t required_count);
        void removePhysical(ConnectionControl& control) noexcept;
    };
} // namespace lux::object::detail
