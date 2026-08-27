#pragma once

#include <concepts>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>
#include <utility>

namespace lux::object
{
    class LuxObject;

    class LUX_CORE_PUBLIC ObjectWeakRef final
    {
    public:
        ObjectWeakRef() noexcept = default;

        [[nodiscard]] bool alive() const noexcept;
        [[nodiscard]] bool expired() const noexcept
        {
            return !alive();
        }
        [[nodiscard]] LuxObject* getOnCurrent() const noexcept;
        [[nodiscard]] ObjectDispatcherRef dispatcherRef() const noexcept;

#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        [[nodiscard]] const void* storageIdentityForTest() const noexcept
        {
            return state_.get();
        }
#endif

        template <class Type>
            requires std::derived_from<Type, LuxObject>
        [[nodiscard]] Type* getAsOnCurrent() const noexcept
        {
            return static_cast<Type*>(getAsOnCurrentErased(lux::cxx::typeToken<Type>()));
        }

    private:
        friend class LuxObject;
        explicit ObjectWeakRef(lux::cxx::intrusive_ptr<detail::ObjectState> state) noexcept : state_(std::move(state))
        {
        }

        [[nodiscard]] LuxObject* getAsOnCurrentErased(lux::cxx::TypeToken type) const noexcept;

        lux::cxx::intrusive_ptr<detail::ObjectState> state_;
    };
} // namespace lux::object
