#include <lux/engine/ecs/System.hpp>

class systemframe_raw_world_negative final : public lux::ecs::System
{
  public:
    void update(lux::ecs::SystemFrame& frame) noexcept override
    {
        (void)frame.world();
    }
};

int main()
{
}
