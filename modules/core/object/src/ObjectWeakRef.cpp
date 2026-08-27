#include <lux/engine/object/ObjectWeakRef.hpp>

#include <cstdlib>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    namespace
    {
        [[noreturn]] void failObjectContract() noexcept
        {
#if defined(_MSC_VER)
            __fastfail(7u);
#else
            std::abort();
#endif
        }
    } // namespace

    bool ObjectWeakRef::alive() const noexcept
    {
        return state_ && state_->object.load(std::memory_order_acquire) != nullptr;
    }

    LuxObject* ObjectWeakRef::getOnCurrent() const noexcept
    {
        if (!state_)
            return nullptr;
#if !defined(NDEBUG) || defined(LUX_OBJECT_CONTRACT_CHECKS)
        if (state_->affinity != std::this_thread::get_id())
            failObjectContract();
#endif
        return state_->object.load(std::memory_order_acquire);
    }

    LuxObject* ObjectWeakRef::getAsOnCurrentErased(lux::cxx::TypeToken type) const noexcept
    {
        auto* object = getOnCurrent();
        return object && object->isObjectType(type) ? object : nullptr;
    }

    ObjectDispatcherRef ObjectWeakRef::dispatcherRef() const noexcept
    {
        return state_ ? state_->dispatcher : ObjectDispatcherRef{};
    }
} // namespace lux::object
