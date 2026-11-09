#pragma once

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>
#include <type_traits>
#include <utility>

namespace lux::object
{
    class LuxObject;

    namespace detail
    {
        using QueuedSignalDelivery = bool (*)(lux::cxx::intrusive_ptr<ConnectionControl>, const void*) noexcept;

        template <class Payload>
        [[nodiscard]] bool
        enqueueSignal(lux::cxx::intrusive_ptr<ConnectionControl> control, const void* payload) noexcept
        {
            auto& execution = execution::ExecutionRuntime::instance();
            auto scheduled = stdexec::schedule(execution.mainScheduler());
            if constexpr (std::is_void_v<Payload>)
            {
                return static_cast<bool>(execution.spawn(
                    std::move(scheduled) | stdexec::then([control = std::move(control)]() noexcept
                    {
                        if (!execution::ExecutionRuntime::instance().isStopping())
                        {
                            invokeConnection(control.get(), nullptr);
                        }
                    })
                ));
            }
            else
            {
                return static_cast<bool>(execution.spawn(
                    std::move(scheduled) | stdexec::then(
                        [control = std::move(control), value = *static_cast<const Payload*>(payload)]() noexcept
                        {
                            if (!execution::ExecutionRuntime::instance().isStopping())
                            {
                                invokeConnection(control.get(), &value);
                            }
                        }
                    )
                ));
            }
        }
    } // namespace detail

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
        [[nodiscard]] static constexpr detail::QueuedSignalDelivery queueDelivery() noexcept
        {
            if constexpr (std::is_void_v<Payload> || std::is_copy_constructible_v<Payload>)
            {
                return &detail::enqueueSignal<Payload>;
            }
            else
            {
                return nullptr;
            }
        }

        LuxObject* owner_;
        lux::cxx::intrusive_ptr<detail::SignalStorage> storage_;
    };
}
