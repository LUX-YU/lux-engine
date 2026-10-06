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

    struct ObjectState final
    {
        ObjectState(LuxObject* value, ObjectId identity) noexcept
            : object(value), id(identity)
        {}
        std::atomic_size_t refs{};
        std::atomic<LuxObject*> object;
        const ObjectId id;
        std::mutex mutex;
        ConnectionControl* incoming{};

        [[nodiscard]] bool addIncoming(ConnectionControl&) noexcept;
        void removeIncoming(ConnectionControl&) noexcept;
        void closeOwner() noexcept;
    };

    struct SignalStorage final
    {
        using Records =
            lux::cxx::StableSlotMap<lux::cxx::intrusive_ptr<ConnectionControl>, ConnectionControl, lux::cxx::NoAux, 8>;
        SignalStorage(QueuedMessageFactory factory) noexcept
            : queue_factory(factory)
        {}
        std::atomic_size_t refs{};
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

    void scheduleSignalMaintenance(SignalStorage&) noexcept;
}
