#include <lux/engine/ecs/SystemConcept.hpp>

class system_missing_access_negative final
{
public:
    void update(lux::ecs::SystemContext&) noexcept {}
};

static_assert(lux::ecs::System<system_missing_access_negative>);
