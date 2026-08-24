#include <lux/engine/ecs/Schedule.hpp>

class schedule_mutable_get_negative final : public lux::ecs::System
{
  public:
    void update(lux::ecs::SystemFrame&) noexcept override {}
};

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule{world};
    lux::ecs::SystemHandle<schedule_mutable_get_negative> handle;
    (void)schedule.get(handle);
}
