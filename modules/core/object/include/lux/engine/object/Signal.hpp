#pragma once

#include <cstddef>
#include <type_traits>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectFwd.hpp>

namespace lux::object
{
    namespace detail
    {
        using QueuedMessageFactory = ObjectMessage (*)(
            lux::cxx::intrusive_ptr<ConnectionControl>,
            const void*
        ) noexcept;

        struct GeneratedSignalAccess;

        LUX_CORE_PUBLIC void invokeQueuedConnection(
            ConnectionControl* control,
            const void* payload
        ) noexcept;

        template <class Payload>
        [[nodiscard]] ObjectMessage makeQueuedSignalMessage(
            lux::cxx::intrusive_ptr<ConnectionControl> control,
            const void* payload
        ) noexcept
        {
            if constexpr (std::is_void_v<Payload>)
            {
                return makeObjectMessage(
                    [control = std::move(control)]
                    { invokeQueuedConnection(control.get(), nullptr); }
                );
            }
            else
            {
                static_assert(std::is_copy_constructible_v<Payload>);
                return makeObjectMessage(
                    [control = std::move(control),
                     value = Payload(*static_cast<const Payload*>(payload))]() mutable
                    { invokeQueuedConnection(control.get(), &value); }
                );
            }
        }
    } // namespace detail

    /** Build-local coordinate in one LuxObject inheritance lineage. */
    class SignalIndex final
    {
    public:
        [[nodiscard]] constexpr std::size_t value() const noexcept
        {
            return value_;
        }

        [[nodiscard]] constexpr bool
        operator==(const SignalIndex&) const noexcept = default;

    private:
        friend struct detail::GeneratedSignalAccess;
        constexpr explicit SignalIndex(std::size_t value) noexcept : value_(value) {}

        std::size_t value_{0};
    };

    struct SignalRuntime final
    {
        SignalIndex index;
        std::size_t lineage_size{0};
        lux::cxx::TypeToken owner;
        lux::cxx::TypeToken payload;
        detail::QueuedMessageFactory queued_message_factory{nullptr};

        [[nodiscard]] bool hasPayload() const noexcept
        {
            return payload != lux::cxx::typeToken<void>();
        }
    };

    template <class Owner, class Payload = void> class Signal final
    {
    public:
        using owner_type = Owner;
        using payload_type = Payload;

        [[nodiscard]] constexpr SignalIndex index() const noexcept
        {
            return runtime_.index;
        }

        [[nodiscard]] constexpr std::size_t lineageSize() const noexcept
        {
            return runtime_.lineage_size;
        }

        [[nodiscard]] constexpr const SignalRuntime& runtime() const noexcept
        {
            return runtime_;
        }

    private:
        friend struct detail::GeneratedSignalAccess;

        constexpr Signal(SignalIndex index, std::size_t lineage_size) noexcept
            : runtime_{
                  index,
                  lineage_size,
                  lux::cxx::typeToken<Owner>(),
                  lux::cxx::typeToken<Payload>(),
                  queueFactory()}
        {
        }

        [[nodiscard]] static consteval detail::QueuedMessageFactory queueFactory()
        {
            if constexpr (
                std::is_void_v<Payload> || std::is_copy_constructible_v<Payload>
            )
            {
                return &detail::makeQueuedSignalMessage<Payload>;
            }
            else
            {
                return nullptr;
            }
        }

        SignalRuntime runtime_;
    };

    namespace detail
    {
        /** Internal code-generation bridge; production source use is gated. */
        struct GeneratedSignalAccess final
        {
            template <class SignalType>
            [[nodiscard]] static consteval SignalType
            make(std::size_t index, std::size_t lineage_size) noexcept
            {
                return SignalType{SignalIndex{index}, lineage_size};
            }
        };
    } // namespace detail

    static_assert(std::is_standard_layout_v<Signal<int, int>>);
    static_assert(sizeof(Signal<int, int>) == sizeof(SignalRuntime));
} // namespace lux::object
