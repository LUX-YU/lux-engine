#pragma once

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>
#include <type_traits>

namespace lux::object
{
    class LuxObject;

    namespace detail
    {
        using QueuedMessageFactory =
            MessageEnvelope (*)(lux::cxx::intrusive_ptr<ConnectionControl>, const void*) noexcept;

        // A fallible value-copy factory. Dispatch itself does not catch foreign exceptions.
        template <class Payload>
        [[nodiscard]] MessageEnvelope makeQueuedSignalMessage(
            lux::cxx::intrusive_ptr<ConnectionControl> control,
            const void* payload
        ) noexcept
        {
            if constexpr (std::is_void_v<Payload>)
                return makeMessage([control = std::move(control)]() noexcept {
                    invokeConnection(control.get(), nullptr);
                });
            else
                return makeMessage([control = std::move(control), value = *static_cast<const Payload*>(payload)](
                                   ) mutable noexcept { invokeConnection(control.get(), &value); });
        }
    }

    template <class Payload = void> class TSignal final
    {
    public:
        using payload_type = Payload;
        explicit TSignal(LuxObject& owner) noexcept : owner_(&owner) {}
        ~TSignal() noexcept
        {
            detail::closeSignal(storage_.get());
        }
        TSignal(const TSignal&) = delete;
        TSignal& operator=(const TSignal&) = delete;
        TSignal(TSignal&&) = delete;
        TSignal& operator=(TSignal&&) = delete;

    private:
        friend class LuxObject;
        [[nodiscard]] static constexpr detail::QueuedMessageFactory queueFactory() noexcept
        {
            if constexpr (std::is_void_v<Payload> || std::is_copy_constructible_v<Payload>)
                return &detail::makeQueuedSignalMessage<Payload>;
            else
                return nullptr;
        }

        LuxObject* owner_;
        lux::cxx::intrusive_ptr<detail::SignalStorage> storage_;
    };
}
