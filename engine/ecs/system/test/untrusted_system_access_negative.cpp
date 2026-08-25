#include <lux/engine/ecs/SystemConcept.hpp>

class untrusted_system_access_negative final
{
public:
    inline static constexpr lux::ecs::SystemAccessSpec Access{};
    void update(lux::ecs::SystemContext&) noexcept {}
};

static_assert(lux::ecs::System<untrusted_system_access_negative>);
