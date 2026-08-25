#include <lux/engine/ecs/SystemConcept.hpp>

class system_throwing_update_negative final
    : public lux::ecs::StaticSystemAccess<>
{
public:
    void update(lux::ecs::SystemContext&) {}
};

static_assert(lux::ecs::System<system_throwing_update_negative>);
