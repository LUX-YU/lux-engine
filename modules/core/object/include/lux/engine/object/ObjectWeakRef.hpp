#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectFwd.hpp>
#include <utility>

namespace lux::object
{
    class LuxObject;

    class LUX_CORE_PUBLIC ObjectWeakRef final
    {
    public:
        ObjectWeakRef() noexcept = default;

        [[nodiscard]] LuxObject* get() const noexcept;
        [[nodiscard]] bool expired() const noexcept { return get() == nullptr; }
        [[nodiscard]] ObjectDispatcherRef dispatcherRef() const noexcept;

        template <class Type> [[nodiscard]] Type* getAs() const noexcept
        {
            return static_cast<Type*>(getAsErased(lux::cxx::typeToken<Type>()));
        }

    private:
        friend class LuxObject;
        explicit ObjectWeakRef(lux::cxx::intrusive_ptr<detail::ObjectState> state
        ) noexcept
            : state_(std::move(state))
        {
        }

        [[nodiscard]] LuxObject* getAsErased(lux::cxx::TypeToken type) const noexcept;

        lux::cxx::intrusive_ptr<detail::ObjectState> state_;
    };
} // namespace lux::object
