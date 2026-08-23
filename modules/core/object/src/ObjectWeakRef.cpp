#include <lux/engine/object/ObjectWeakRef.hpp>

#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    LuxObject* ObjectWeakRef::get() const noexcept
    {
        const auto control = control_.lock();
        return control ? control->object.load(std::memory_order_acquire) : nullptr;
    }
}
