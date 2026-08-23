#pragma once

#include <atomic>
#include <concepts>
#include <cstdlib>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectFwd.hpp>
#include <lux/engine/object/ObjectWeakRef.hpp>
#include <lux/engine/object/Signal.hpp>

namespace lux::object
{
    class EventView;
    class LuxObject;

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
        OBJECT_CLOSED,
        PAYLOAD_NOT_QUEUEABLE
    };

    namespace detail
    {
        using ObjectInvokeThunk = void (*)(LuxObject*, const void*, void*);
        [[nodiscard]] LUX_CORE_PUBLIC bool
        sendEventErased(LuxObject& target, EventView& event);

        [[nodiscard]] LUX_CORE_PUBLIC lux::cxx::expected<Connection, EObserveError>
        observeDynamicErased(
            LuxObject& sender,
            const SignalRuntime& signal,
            LuxObject& receiver,
            ObjectInvokeThunk invoke,
            std::shared_ptr<void> context,
            EDelivery delivery
        );

        template <class Method> struct MemberMethodTraits;

        template <class Owner, class Result, class... Args>
        struct MemberMethodTraits<Result (Owner::*)(Args...)>
        {
            using owner_type = Owner;
            using result_type = Result;
            static constexpr std::size_t arity = sizeof...(Args);
            using arguments = std::tuple<Args...>;
        };

        template <class Owner, class Result, class... Args>
        struct MemberMethodTraits<Result (Owner::*)(Args...) const>
            : MemberMethodTraits<Result (Owner::*)(Args...)>
        {
        };

        template <class Owner, class Result, class... Args>
        struct MemberMethodTraits<Result (Owner::*)(Args...) noexcept>
            : MemberMethodTraits<Result (Owner::*)(Args...)>
        {
        };

        template <class Owner, class Result, class... Args>
        struct MemberMethodTraits<Result (Owner::*)(Args...) const noexcept>
            : MemberMethodTraits<Result (Owner::*)(Args...)>
        {
        };
    } // namespace detail

    class LUX_CORE_PUBLIC LuxObject
    {
    public:
        explicit LuxObject(ObjectDispatcherRef dispatcher = {}) noexcept;
        virtual ~LuxObject();

        LuxObject(const LuxObject&) = delete;
        LuxObject& operator=(const LuxObject&) = delete;
        LuxObject(LuxObject&&) = delete;
        LuxObject& operator=(LuxObject&&) = delete;

        [[nodiscard]] virtual lux::cxx::TypeToken objectType() const noexcept;
        [[nodiscard]] virtual bool isObjectType(lux::cxx::TypeToken type
        ) const noexcept;
        [[nodiscard]] ObjectWeakRef weakRef() const;
        [[nodiscard]] std::thread::id affinity() const noexcept { return affinity_; }
        [[nodiscard]] const ObjectDispatcherRef& dispatcherRef() const noexcept
        {
            return dispatcher_;
        }

        template <auto& StaticSignal, class Payload> void notify(Payload&& payload)
        {
            using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
            using DeclaredPayload = typename SignalType::payload_type;
            static_assert(!std::is_void_v<DeclaredPayload>);
            static_assert(
                std::same_as<std::remove_cvref_t<Payload>, DeclaredPayload>,
                "Signal payload must exactly match its declaration"
            );
            notifyErased(StaticSignal.runtime(), std::addressof(payload));
        }

        template <auto& StaticSignal> void notify()
        {
            using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
            static_assert(std::is_void_v<typename SignalType::payload_type>);
            notifyErased(StaticSignal.runtime(), nullptr);
        }

        template <auto& StaticSignal, auto Method, EDelivery Delivery = EDelivery::AUTO, class Receiver>
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError>
        observe(Receiver& receiver) requires std::derived_from<Receiver, LuxObject>
        {
            using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
            using Payload    = typename SignalType::payload_type;
            using Traits     = detail::MemberMethodTraits<decltype(Method)>;
            static_assert(std::same_as<typename Traits::result_type, void>);
            static_assert(std::is_base_of_v<typename Traits::owner_type, Receiver>);
            if constexpr (std::is_void_v<Payload>)
            {
                static_assert(Traits::arity == 0);
            }
            else
            {
                static_assert(Traits::arity == 1);
                static_assert(std::same_as<std::tuple_element_t<0, typename Traits::arguments>, const Payload&>);
                if constexpr (Delivery != EDelivery::DIRECT)
                {
                    static_assert(
                        std::copy_constructible<Payload>,
                        "Queued-capable Signal payload must be copyable"
                    );
                }
            }

            return observeErased(
                StaticSignal.runtime(),
                receiver,
                [](LuxObject* object, const void* payload, void*)
                {
                    auto& typed_receiver = *static_cast<Receiver*>(object);
                    if constexpr (std::is_void_v<Payload>)
                        (typed_receiver.*Method)();
                    else
                        (typed_receiver.*Method)(*static_cast<const Payload*>(payload));
                },
                Delivery,
                StaticSignal.runtime().queued_message_factory,
                {}
            );
        }

        template <auto& StaticSignal, auto Function>
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observe()
        {
            using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
            using Payload = typename SignalType::payload_type;
            if constexpr (std::is_void_v<Payload>)
                static_assert(std::same_as<decltype(Function), void (*)()>);
            else
                static_assert(std::same_as<decltype(Function), void (*)(const Payload&)>);

            return observeStaticErased(
                StaticSignal.runtime(),
                [](LuxObject*, const void* payload, void*)
                {
                    if constexpr (std::is_void_v<Payload>)
                        Function();
                    else
                        Function(*static_cast<const Payload*>(payload));
                }
            );
        }

        template <auto& StaticSignal, class Callable>
        [[nodiscard]] lux::cxx::expected<ScopedConnection, EObserveError>
        observeScoped(Callable&& callable)
        {
            using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
            using Payload = typename SignalType::payload_type;
            using Stored = std::remove_cvref_t<Callable>;
            if constexpr (std::is_void_v<Payload>)
                static_assert(std::is_invocable_r_v<void, Stored&>);
            else
                static_assert(std::is_invocable_r_v<void, Stored&, const Payload&>);

            auto holder = std::make_shared<Stored>(std::forward<Callable>(callable));
            auto result = observeCallableErased(
                StaticSignal.runtime(),
                [](LuxObject*, const void* payload, void* context)
                {
                    auto& function = *static_cast<Stored*>(context);
                    if constexpr (std::is_void_v<Payload>)
                        function();
                    else
                        function(*static_cast<const Payload*>(payload));
                },
                std::move(holder)
            );
            if (!result)
            {
                return lux::cxx::unexpected<EObserveError>{result.error()};
            }
            return ScopedConnection{std::move(*result)};
        }

    protected:
        virtual void event(EventView&) {}

    private:
        friend class ObjectWeakRef;
        friend bool detail::sendEventErased(LuxObject&, EventView&);
        friend lux::cxx::expected<Connection, EObserveError>
        detail::observeDynamicErased(
            LuxObject&,
            const SignalRuntime&,
            LuxObject&,
            detail::ObjectInvokeThunk,
            std::shared_ptr<void>,
            EDelivery
        );

        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observeErased(
            const SignalRuntime& signal,
            LuxObject& receiver,
            detail::ObjectInvokeThunk invoke,
            EDelivery delivery,
            detail::QueuedMessageFactory queue_factory,
            std::shared_ptr<void> context
        );
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError> observeStaticErased(
            const SignalRuntime& signal,
            detail::ObjectInvokeThunk invoke
        );
        [[nodiscard]] lux::cxx::expected<Connection, EObserveError>
        observeCallableErased(
            const SignalRuntime& signal,
            detail::ObjectInvokeThunk invoke,
            std::shared_ptr<void> context
        );
        void notifyErased(const SignalRuntime& signal, const void* payload);
        [[nodiscard]] lux::cxx::intrusive_ptr<detail::ObjectState> ensureState() const;

        mutable std::atomic<detail::ObjectState*> state_{nullptr};
        std::thread::id affinity_;
        ObjectDispatcherRef dispatcher_;
    };
} // namespace lux::object
