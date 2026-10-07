#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/ObjectScheduler.hpp>

namespace lux::process
{
    ObjectScheduler::ObjectScheduler(object::ObjectTarget target) noexcept : target_(std::move(target)) {}
    ObjectScheduler objectScheduler(object::LuxObject& target) noexcept
    {
        return ObjectScheduler{target.target()};
    }
} // namespace lux::process
