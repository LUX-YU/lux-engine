#pragma once

#include <memory>
#include <utility>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>

namespace lux::object
{
    class LuxObject;

    class EventView final
    {
    public:
        template <class Event>
        explicit EventView(Event& event) noexcept : type_(lux::cxx::typeToken<Event>()), data_(std::addressof(event))
        {}

        [[nodiscard]] lux::cxx::TypeToken type() const noexcept
        {
            return type_;
        }

        template <class Event> [[nodiscard]] Event* getIf() const noexcept
        {
            return type_ == lux::cxx::typeToken<Event>() ? static_cast<Event*>(data_) : nullptr;
        }

        void accept() noexcept
        {
            accepted_ = true;
        }
        [[nodiscard]] bool accepted() const noexcept
        {
            return accepted_;
        }

    private:
        lux::cxx::TypeToken type_;
        void* data_{nullptr};
        bool accepted_{false};
    };

    namespace detail
    {
        [[nodiscard]] LUX_CORE_PUBLIC bool sendEventErased(LuxObject& target, EventView& event) noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC bool routeEventErased(
            LuxObject& target,
            LuxObject& boundary,
            EventView& event
        ) noexcept;
    }

    [[nodiscard]] inline bool sendEvent(LuxObject& target, EventView& event) noexcept
    {
        return detail::sendEventErased(target, event);
    }

    template <class Event> [[nodiscard]] bool sendEvent(LuxObject& target, Event& event) noexcept
    {
        EventView view{event};
        return detail::sendEventErased(target, view);
    }

    // Ancestors filter from boundary down, then unaccepted events bubble from target up.
    // The boundary must belong to the target's parent chain on this thread.
    [[nodiscard]] inline bool routeEvent(LuxObject& target, LuxObject& boundary, EventView& event) noexcept
    {
        return detail::routeEventErased(target, boundary, event);
    }

    template <class Event> [[nodiscard]] bool routeEvent(LuxObject& target, LuxObject& boundary, Event& event) noexcept
    {
        EventView view{event};
        return detail::routeEventErased(target, boundary, view);
    }

} // namespace lux::object
