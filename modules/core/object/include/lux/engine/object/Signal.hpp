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
        );

        LUX_CORE_PUBLIC void invokeQueuedConnection(
            ConnectionControl* control,
            const void* payload
        ) noexcept;

        template <class Payload>
        [[nodiscard]] ObjectMessage makeQueuedSignalMessage(
            lux::cxx::intrusive_ptr<ConnectionControl> control,
            const void* payload
        )
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
    struct SignalIndex final
    {
        std::size_t value{0};

        [[nodiscard]] constexpr bool
        operator==(const SignalIndex&) const noexcept = default;
    };

    struct SignalRuntime final
    {
        SignalIndex index;
        lux::cxx::TypeToken owner;
        lux::cxx::TypeToken payload;
        detail::QueuedMessageFactory queued_message_factory{nullptr};
        bool has_payload{false};
    };

    template <class Owner, class Payload = void> class Signal final
    {
    public:
        using owner_type = Owner;
        using payload_type = Payload;

        constexpr explicit Signal(SignalIndex index) noexcept
            : runtime_{
                  index,
                  lux::cxx::typeToken<Owner>(),
                  lux::cxx::typeToken<Payload>(),
                  queueFactory(),
                  !std::is_void_v<Payload>}
        {
        }

        [[nodiscard]] constexpr SignalIndex index() const noexcept
        {
            return runtime_.index;
        }

        [[nodiscard]] constexpr const SignalRuntime& runtime() const noexcept
        {
            return runtime_;
        }

    private:
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

    static_assert(std::is_standard_layout_v<Signal<int, int>>);
} // namespace lux::object
