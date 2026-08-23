#pragma once

#include <memory>

#include <lux/cxx/compile_time/TypeToken.hpp>

namespace lux::object
{
    class EventView final
    {
      public:
        template<typename Event>
        explicit EventView(Event& event) noexcept
            : type_(lux::cxx::typeToken<Event>()), data_(std::addressof(event))
        {
        }

        [[nodiscard]] lux::cxx::TypeToken type() const noexcept { return type_; }

        template<typename Event>
        [[nodiscard]] Event* getIf() const noexcept
        {
            return type_ == lux::cxx::typeToken<Event>()
                ? static_cast<Event*>(data_)
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
}
