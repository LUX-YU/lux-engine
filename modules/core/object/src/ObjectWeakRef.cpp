#include <lux/engine/object/ObjectWeakRef.hpp>

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    LuxObject* ObjectWeakRef::get() const noexcept
    {
        return state_ ? state_->object.load(std::memory_order_acquire) : nullptr;
    }

    LuxObject* ObjectWeakRef::getAsErased(lux::cxx::TypeToken type) const noexcept
    {
        auto* object = get();
        return object && object->isObjectType(type) ? object : nullptr;
    }

    ObjectDispatcherRef ObjectWeakRef::dispatcherRef() const noexcept
    {
        return state_ ? state_->dispatcher : ObjectDispatcherRef{};
    }
} // namespace lux::object
