#pragma once

#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>
#include <concepts>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectWeakRef.hpp>
#include <lux/engine/object/Signal.hpp>

namespace lux::object
{
    enum class EDelivery
    {
        DIRECT,
        QUEUED,
        AUTO
    };

    enum class EObserveError
    {
        WRONG_SIGNAL_OWNER,
        RECEIVER_HAS_NO_DISPATCHER,
        DIRECT_CROSS_AFFINITY,
        OBJECT_CLOSED
    };

    namespace detail
    {
        struct ObjectState;
        struct ObjectControl;
    }

    class LUX_CORE_PUBLIC LuxObject
    {
      public:
        LuxObject() noexcept;
        virtual ~LuxObject();

        LuxObject(const LuxObject&) = delete;
        LuxObject& operator=(const LuxObject&) = delete;
        LuxObject(LuxObject&&) = delete;
        LuxObject& operator=(LuxObject&&) = delete;

        [[nodiscard]] virtual lux::cxx::TypeToken objectType() const noexcept;
        [[nodiscard]] virtual bool isObjectType(
            lux::cxx::TypeToken type
        ) const noexcept;
        [[nodiscard]] ObjectWeakRef weakRef() const;
        [[nodiscard]] std::thread::id affinity() const noexcept { return affinity_; }

        void setDispatcher(ObjectDispatcher* dispatcher);
        [[nodiscard]] ObjectDispatcher* dispatcher() const noexcept { return dispatcher_; }

        template<typename Owner, typename Payload, typename Callable>
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observe(
            const Signal<Owner, Payload>& signal,
            Callable&& callable
        )
        {
            static_assert(std::is_invocable_r_v<void, Callable&, const Payload&>);
            return observeErased(
                signal.header(),
                lux::cxx::move_only_function<void(const void*)>{
                    [fn = std::forward<Callable>(callable)](const void* payload) mutable
                    {
                        fn(*static_cast<const Payload*>(payload));
                    }
                },
                nullptr,
                EDelivery::DIRECT
            );
        }

        template<typename Owner, typename Payload, typename Receiver>
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observe(
            const Signal<Owner, Payload>& signal,
            Receiver& receiver,
            void (Receiver::*method)(const Payload&),
            EDelivery delivery = EDelivery::AUTO
        )
        requires std::derived_from<Receiver, LuxObject>
        {
            return observeErased(
                signal.header(),
                lux::cxx::move_only_function<void(const void*)>{
                    [&receiver, method](const void* payload)
                    {
                        (receiver.*method)(*static_cast<const Payload*>(payload));
                    }
                },
                &receiver,
                delivery
            );
        }

        template<typename Owner, typename Payload>
        void emit(const Signal<Owner, Payload>& signal, const Payload& payload)
        {
            static_assert(
                std::is_copy_constructible_v<Payload>,
                "Queued-capable object signals require copyable payloads"
            );
            notifyErased(
                signal.header(),
                std::addressof(payload),
                [](const void* value) -> std::shared_ptr<const void>
                {
                    return std::make_shared<Payload>(
                        *static_cast<const Payload*>(value)
                    );
                }
            );
        }

        template<typename Owner>
        void emit(const Signal<Owner, NoSignalPayload>& signal)
        {
            static constexpr NoSignalPayload payload{};
            notifyErased(
                signal.header(),
                std::addressof(payload),
                [](const void* value) -> std::shared_ptr<const void>
                {
                    return std::make_shared<NoSignalPayload>(
                        *static_cast<const NoSignalPayload*>(value)
                    );
                }
            );
        }

        [[nodiscard]] lux::cxx::expected<Connection, EObserveError>
        observeDynamic(
            const SignalHeader& signal,
            lux::cxx::move_only_function<void(const void*)> callback,
            LuxObject& receiver,
            EDelivery delivery = EDelivery::AUTO
        );

        template<typename Event>
        bool sendEvent(Event& event_value)
        {
            EventView view{event_value};
            event(view);
            return view.accepted();
        }

        template<typename Event>
        [[nodiscard]] EEventPostStatus postEvent(Event event_value)
        {
            auto* target_dispatcher = dispatcher_;
            if (!target_dispatcher) return EEventPostStatus::NO_DISPATCHER;
            auto target = weakRef();
            auto shared_event = std::make_shared<Event>(std::move(event_value));
            const auto status = target_dispatcher->post(
                [target, shared_event = std::move(shared_event)]() mutable
                {
                    if (auto* object = target.get())
                        static_cast<void>(object->sendEvent(*shared_event));
                }
            );
            return status == EPostStatus::POSTED
                ? EEventPostStatus::POSTED
                : EEventPostStatus::CLOSED;
        }

      protected:
        virtual void event(EventView&) {}

      private:
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observeErased(
            const SignalHeader& signal,
            lux::cxx::move_only_function<void(const void*)> callback,
            LuxObject* receiver,
            EDelivery delivery
        );
        using PayloadClone = std::shared_ptr<const void> (*)(const void*);
        void notifyErased(
            const SignalHeader& signal,
            const void* payload,
            PayloadClone clone_payload
        );
        [[nodiscard]] std::shared_ptr<detail::ObjectState> ensureState() const;

        mutable std::shared_ptr<detail::ObjectState> state_;
        std::thread::id affinity_;
        ObjectDispatcher* dispatcher_{nullptr};
    };
}
