#pragma once

#include <memory>

#include <lux/engine/core/visibility.h>

namespace lux::object
{
    class LuxObject;

    namespace detail
    {
        struct ObjectControl;
    }

    class LUX_CORE_PUBLIC ObjectWeakRef final
    {
      public:
        ObjectWeakRef() noexcept = default;

        [[nodiscard]] LuxObject* get() const noexcept;
        [[nodiscard]] bool expired() const noexcept { return get() == nullptr; }

        template<typename Type>
        [[nodiscard]] Type* getAs() const noexcept
        {
            return static_cast<Type*>(get());
        }

      private:
        friend class LuxObject;
        explicit ObjectWeakRef(std::weak_ptr<detail::ObjectControl> control) noexcept
            : control_(std::move(control))
        {
        }

        std::weak_ptr<detail::ObjectControl> control_;
    };
}
