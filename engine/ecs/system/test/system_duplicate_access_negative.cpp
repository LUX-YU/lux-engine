#include <lux/engine/ecs/SystemConcept.hpp>

struct Position final
{
};

class system_duplicate_access_negative final
    : public lux::ecs::StaticSystemAccess<
        lux::ecs::Read<Position>,
        lux::ecs::Write<Position>
    >
{
public:
    void update(lux::ecs::SystemContext&) noexcept {}
};

static_assert(lux::ecs::System<system_duplicate_access_negative>);
