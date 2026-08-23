#pragma once

#include <memory>
#include <utility>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectWeakRef.hpp>

namespace lux::object
{
    class LuxObject;

    class EventView final
    {
    public:
        template <class Event>
        explicit EventView(Event& event) noexcept
            : type_(lux::cxx::typeToken<Event>()), data_(std::addressof(event))
        {
        }

        [[nodiscard]] lux::cxx::TypeToken type() const noexcept { return type_; }

        template <class Event> [[nodiscard]] Event* getIf() const noexcept
        {
            return type_ == lux::cxx::typeToken<Event>() ? static_cast<Event*>(data_)
                                                         : nullptr;
        }

        void accept() noexcept { accepted_ = true; }
        [[nodiscard]] bool accepted() const noexcept { return accepted_; }

    private:
        lux::cxx::TypeToken type_;
        void* data_{nullptr};
        bool accepted_{false};
    };

    enum class EEventPostStatus
    {
        POSTED,
        NO_DISPATCHER,
        CLOSED
    };

    namespace detail
    {
        [[nodiscard]] LUX_CORE_PUBLIC bool
        sendEventErased(LuxObject& target, EventView& event) noexcept;
    }

    template <class Event>
    [[nodiscard]] bool sendEvent(LuxObject& target, Event& event) noexcept
    {
        EventView view{event};
        return detail::sendEventErased(target, view);
    }

    template <class Event>
    [[nodiscard]] EEventPostStatus postEvent(ObjectWeakRef target, Event event)
    {
        const auto dispatcher = target.dispatcherRef();

        if (!dispatcher)
        {
            return EEventPostStatus::NO_DISPATCHER;
        }

        auto message = detail::makeObjectMessage(
            [target = std::move(target), event = std::move(event)]() mutable
            {
                if (auto* object = target.getOnCurrent())
                    static_cast<void>(sendEvent(*object, event));
            }
        );

        return dispatcher.post(std::move(message)) == EPostStatus::POSTED
                   ? EEventPostStatus::POSTED
                   : EEventPostStatus::CLOSED;
    }
} // namespace lux::object
