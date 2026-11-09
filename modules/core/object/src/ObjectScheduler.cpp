#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectScheduler.hpp>

namespace lux::object
{
    ObjectScheduler::ObjectScheduler(object::ObjectTarget target) noexcept : target_(std::move(target)) {}

    ObjectScheduler objectScheduler(object::LuxObject& target) noexcept
    {
        return ObjectScheduler{target.target()};
    }
} // namespace lux::object
