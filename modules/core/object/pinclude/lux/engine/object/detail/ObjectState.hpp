#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/detail/Reclamation.hpp>
#include <mutex>

namespace lux::object::detail
{
    [[noreturn]] void failObjectContract() noexcept;
    struct ConnectionControl final
    {
        std::atomic_size_t refs{};
        std::atomic_bool connected{true};
        lux::cxx::intrusive_ptr<SignalStorage> storage;
        lux::cxx::intrusive_ptr<ObjectState> receiver;
        lux::cxx::SlotKey<ConnectionControl> key;
        SignalCallback callback;
        EDelivery delivery{EDelivery::DIRECT};
        ConnectionControl* previous_incoming{};
        ConnectionControl* next_incoming{};
        ConnectionControl* next_cancelled{};
        bool incoming_linked{}; // Receiver mutex only.
    };

    struct ObjectState final : Reclamation
    {
        ObjectState(LuxObject* value, ObjectDispatcherRef queue) noexcept : object(value), dispatcher(std::move(queue))
        {}
        std::atomic_size_t refs{};
        std::atomic<LuxObject*> object;
        ObjectDispatcherRef dispatcher;
        std::mutex mutex;
        ConnectionControl* incoming{};
        bool destruction_requested{}; // Affinity thread; one queued reference per identity.

        [[nodiscard]] bool addIncoming(ConnectionControl&) noexcept;
        void removeIncoming(ConnectionControl&) noexcept;
        void closeOwner() noexcept;
        static bool reclaimOwner(Reclamation&) noexcept;
    };

    struct SignalStorage final
    {
        using Records =
            lux::cxx::StableSlotMap<lux::cxx::intrusive_ptr<ConnectionControl>, ConnectionControl, lux::cxx::NoAux, 8>;
        SignalStorage(ObjectDispatcherRef queue, QueuedMessageFactory factory) noexcept
            : dispatcher(std::move(queue)), queue_factory(factory)
        {}
        std::atomic_size_t refs{};
        const std::thread::id affinity{std::this_thread::get_id()};
        ObjectDispatcherRef dispatcher;
        QueuedMessageFactory queue_factory;
        Records records;
        std::atomic_bool closed{};
        std::size_t depth{}, visible_count{};
        bool maintaining{};
        std::mutex cancel_mutex;
        ConnectionControl* cancelled{};
        SignalStorage* next_maintenance{}; // Dispatcher mutex only, with one owning reference.
        bool maintenance_queued{};

        void cancel(ConnectionControl&) noexcept;
        void remove(ConnectionControl&) noexcept;
        void maintain() noexcept;
        void close() noexcept;
        [[nodiscard]] SignalDelivery emit(const void*) noexcept;
    };

    void scheduleSignalMaintenance(const ObjectDispatcherRef&, SignalStorage&) noexcept;
}
