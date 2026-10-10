#pragma once

#include <atomic>
#include <concepts>
#include <exception>
#include <functional>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectTarget.hpp>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/ObjectOwnership.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/object/Signal.hpp>
#include <memory>
#include <tuple>
#include <type_traits>

namespace lux::object
{
    class EventView;
    class LuxObject;

    // Broadcast admission can be partial; never replay already delivered recipients.
    struct SignalDelivery final
    {
        std::size_t direct{}, queued{}, full{}, closed{};
        [[nodiscard]] bool complete() const noexcept
        {
            return full == 0 && closed == 0;
        }
    };

    enum class EDelivery : std::uint8_t
    {
        DIRECT,
        QUEUED,
        AUTO
    };
    enum class EConnectError : std::uint8_t
    {
        INVALID_ARGUMENT,
        OBJECT_CLOSED,
        WRONG_THREAD,
        PAYLOAD_NOT_QUEUEABLE,
        CAPACITY_EXHAUSTED,
        CALLBACK_CONSTRUCTION_FAILURE
    };

    namespace detail
    {
        using SignalCallback = lux::cxx::move_only_function<void(LuxObject*, const void*)>;
        template <class Method> struct TCallbackArguments;
        template <class Result, class... Args> struct TCallbackArguments<Result (*)(Args...) noexcept>
        {
            using result = Result;
            using arguments = std::tuple<Args...>;
        };
        template <class Owner, class Result, class... Args>
        struct TCallbackArguments<Result (Owner::*)(Args...) noexcept>
            : TCallbackArguments<Result (*)(Args...) noexcept>
        {
        };
        template <class Owner, class Result, class... Args>
        struct TCallbackArguments<Result (Owner::*)(Args...) const noexcept>
            : TCallbackArguments<Result (*)(Args...) noexcept>
        {
        };

        template <class Payload, class Callable> consteval bool validCallbackArguments()
        {
            if constexpr (requires { typename TCallbackArguments<Callable>::arguments; })
            {
                using Traits = TCallbackArguments<Callable>;
                using Args = typename Traits::arguments;
                if constexpr (!std::same_as<typename Traits::result, void>)
                {
                    return false;
                }
                else if constexpr (std::is_void_v<Payload>)
                {
                    return std::tuple_size_v<Args> == 0;
                }
                else if constexpr (std::tuple_size_v<Args> != 1)
                {
                    return false;
                }
                else
                {
                    return std::same_as<std::tuple_element_t<0, Args>, Payload> ||
                           std::same_as<std::tuple_element_t<0, Args>, const Payload&>;
                }
            }
            else if constexpr (requires { &Callable::operator(); })
            {
                return validCallbackArguments<Payload, decltype(&Callable::operator())>();
            }
            else
            {
                return true; // Generic/overloaded callable is checked at the exact invocation below.
            }
        }

        [[nodiscard]] LUX_CORE_PUBLIC bool sendEventErased(LuxObject&, EventView&) noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC bool routeEventErased(LuxObject&, LuxObject&, EventView&) noexcept;
    } // namespace detail

    class LUX_CORE_PUBLIC LuxObject
    {
    public:
        using lux_thread_affine = std::true_type;
        using ConnectResult = lux::cxx::expected<Connection, EConnectError>;

        LuxObject() noexcept;
        virtual ~LuxObject();
        LuxObject(const LuxObject&) = delete;
        LuxObject& operator=(const LuxObject&) = delete;
        LuxObject(LuxObject&&) = delete;
        LuxObject& operator=(LuxObject&&) = delete;

        [[nodiscard]] ObjectTarget target() const noexcept;
        [[nodiscard]] bool isOnAffinityThread() const noexcept;
        [[nodiscard]] LuxObject* parent() const noexcept
        {
            return parent_;
        }

        [[nodiscard]] LuxObject* firstChild() const noexcept
        {
            return first_child_;
        }

        [[nodiscard]] LuxObject* nextSibling() const noexcept
        {
            return next_sibling_;
        }

        [[nodiscard]] ObjectId objectId() const noexcept
        {
            return id_;
        }

        [[nodiscard]] ObjectResult<void> setParent(LuxObject*) noexcept;
        [[nodiscard]] ObjectResult<void> addChild(LuxObject&) noexcept;
        [[nodiscard]] ObjectResult<void> removeChild(LuxObject&) noexcept;

        template <class Sender, class Owner, class Payload, class Callback>
            requires std::derived_from<Sender, Owner> && std::derived_from<Sender, LuxObject>
        [[nodiscard]] static ConnectResult connect(
            Sender* sender,
            TSignal<Payload> Owner::* member,
            Callback&& callback
        ) noexcept
        {
            return connectImpl(
                sender,
                member,
                static_cast<LuxObject*>(nullptr),
                std::forward<Callback>(callback),
                EDelivery::DIRECT
            );
        }

        template <class Sender, class Owner, class Payload, class Receiver, class Callback>
            requires std::derived_from<Sender, Owner> && std::derived_from<Sender, LuxObject> &&
                     std::derived_from<Receiver, LuxObject>
        [[nodiscard]] static ConnectResult connect(
            Sender* sender,
            TSignal<Payload> Owner::* member,
            Receiver* receiver,
            Callback&& callback,
            EDelivery delivery = EDelivery::AUTO
        ) noexcept
        {
            if (!receiver)
            {
                return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
            }
            return connectImpl(sender, member, receiver, std::forward<Callback>(callback), delivery);
        }

    private:
        template <class Sender, class Owner, class Payload, class Receiver, class Callback>
        [[nodiscard]] static ConnectResult connectImpl(
            Sender* sender,
            TSignal<Payload> Owner::* member,
            Receiver* receiver,
            Callback&& callback,
            EDelivery delivery
        ) noexcept
        {
            using Stored = std::remove_cvref_t<Callback>;
            static_assert(detail::validCallbackArguments<Payload, Stored>(), "Signal callback payload must match");
            constexpr bool method = std::is_member_function_pointer_v<Stored>;
            if constexpr (method)
            {
                if constexpr (std::is_void_v<Payload>)
                {
                    static_assert(std::is_nothrow_invocable_v<Stored&, Receiver&> && std::same_as<std::invoke_result_t<Stored&, Receiver&>, void>);
                }
                else
                {
                    static_assert(std::is_nothrow_invocable_v<Stored&, Receiver&, const Payload&> && std::same_as<std::invoke_result_t<Stored&, Receiver&, const Payload&>, void>);
                }
            }
            else
            {
                if constexpr (std::is_void_v<Payload>)
                {
                    static_assert(std::is_nothrow_invocable_v<Stored&> && std::same_as<std::invoke_result_t<Stored&>, void>);
                }
                else
                {
                    static_assert(std::is_nothrow_invocable_v<Stored&, const Payload&> && std::same_as<std::invoke_result_t<Stored&, const Payload&>, void>);
                }
            }
            if (!sender || !member || (method && !receiver))
            {
                return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
            }
            if (!sender->isOnAffinityThread())
            {
                return lux::cxx::unexpected(EConnectError::WRONG_THREAD);
            }
            auto& signal = sender->*member;
            if (signal.owner_ != sender)
            {
                return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
            }
            // Connection creation is the fallible allocation/callable construction boundary.
            try
            {
                detail::SignalCallback invoke{
                    [value = Stored(std::forward<Callback>(callback)
                     )](LuxObject* object, const void* payload) mutable noexcept
                    {
                        if constexpr (method)
                        {
                            auto& target = *static_cast<Receiver*>(object);
                            if constexpr (std::is_void_v<Payload>)
                            {
                                std::invoke(value, target);
                            }
                            else
                            {
                                std::invoke(value, target, *static_cast<const Payload*>(payload));
                            }
                        }
                        else
                        {
                            if constexpr (std::is_void_v<Payload>)
                            {
                                std::invoke(value);
                            }
                            else
                            {
                                std::invoke(value, *static_cast<const Payload*>(payload));
                            }
                        }
                    }
                };
                return sender->connectSignal(
                    signal.storage_,
                    receiver,
                    TSignal<Payload>::queueFactory(),
                    delivery,
                    std::move(invoke)
                );
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return lux::cxx::unexpected(EConnectError::CALLBACK_CONSTRUCTION_FAILURE);
            }
        }

    protected:
        // Typed owners validate their topology before committing the common non-owning relation.
        [[nodiscard]] static ObjectResult<void> validateRelation(LuxObject& child, LuxObject* parent) noexcept;
        static void commitRelation(LuxObject& child, LuxObject* parent) noexcept;
        void clearChildren() noexcept;
        // Revoke callbacks before a typed owner tears down its derived routing/resources.
        void beginDestruction() noexcept;
        [[nodiscard]] bool isClosing() const noexcept
        {
            return closing_;
        }

        [[nodiscard]] SignalDelivery emit(TSignal<>& signal) noexcept
        {
            return emitSignal(signal.owner_, signal.storage_.get(), nullptr);
        }

        template <class Payload>
        [[nodiscard]] SignalDelivery emit(TSignal<Payload>& signal, const Payload& value) noexcept
        {
            return emitSignal(signal.owner_, signal.storage_.get(), std::addressof(value));
        }

        virtual bool allowsGenericStructure() const noexcept
        {
            return true;
        }

        void beginTreeVisit() noexcept;
        void endTreeVisit() noexcept;
        // Owner callbacks may change finished child subtrees, but cannot reclaim themselves or ancestors.
        static void beginCallbackBorrow(LuxObject&) noexcept;
        static void endCallbackBorrow(LuxObject&) noexcept;
        // Thread-local dispatch fact, including direct events and queued/direct signal callbacks.
        // Hosts use this to keep structural adoption outside borrowed callback stacks.
        [[nodiscard]] static bool isDispatching() noexcept;
        virtual void event(EventView&) noexcept {}

        virtual void filterEvent(LuxObject&, EventView&) noexcept {}

    private:
        void linkChild(LuxObject&) noexcept;
        [[nodiscard]] bool hasActiveTree() const noexcept;
        [[nodiscard]] bool acceptsCallbacks() const noexcept;
        void unlinkParent() noexcept;
        friend struct detail::ObjectState;
        friend class ObjectRuntime;
        friend struct detail::AffinityOwner;
        friend ObjectResult<void> detail::prepareSharedObject(LuxObject&) noexcept;
        friend void detail::finishSharedObject(LuxObject&) noexcept;

        friend void detail::invokeConnection(detail::ConnectionControl*, const void*) noexcept;
        friend bool detail::sendEventErased(LuxObject&, EventView&) noexcept;
        friend bool detail::routeEventErased(LuxObject&, LuxObject&, EventView&) noexcept;
        void assertAffinity() const noexcept;
        void filterAncestors(LuxObject&, LuxObject&, EventView&) noexcept;
        [[nodiscard]] lux::cxx::intrusive_ptr<detail::ObjectState> ensureState() const;
        [[nodiscard]] ConnectResult connectSignal(
            lux::cxx::intrusive_ptr<detail::SignalStorage>&,
            LuxObject*,
            detail::QueuedMessageFactory,
            EDelivery,
            detail::SignalCallback
        ) noexcept;
        [[nodiscard]] SignalDelivery emitSignal(LuxObject*, detail::SignalStorage*, const void*) noexcept;

        mutable std::atomic<detail::ObjectState*> state_{nullptr};
        ObjectId id_;
        LuxObject* parent_{};
        LuxObject* first_child_{};
        LuxObject* last_child_{};
        LuxObject* previous_sibling_{};
        LuxObject* next_sibling_{};
        std::size_t active_events_{};
        std::size_t callback_borrows_{};
        bool changing_children_{};
        bool closing_{};
    };
} // namespace lux::object
