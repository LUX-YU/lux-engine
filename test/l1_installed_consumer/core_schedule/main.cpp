#include <lux/engine/ecs/Schedule.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    class CountSystem final : public lux::ecs::System
    {
      public:
        explicit CountSystem(std::uint32_t& count) noexcept : count_(&count) {}

        void update(lux::ecs::SystemFrame&) noexcept override
        {
            ++*count_;
        }

      private:
        std::uint32_t* count_{};
    };
}

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule{
        world,
        lux::ecs::ScheduleConfig{
            lux::ecs::ChangeScratchPolicy{64U, 64U}}
    };
    if (schedule.changeStats().capacity_records != 64U)
        return 1;
    std::uint32_t count{};
    auto edit_result = schedule.edit();
    if (!edit_result)
        return 2;
    auto edit = std::move(*edit_result);
    if (!edit.add(std::make_unique<CountSystem>(count)) ||
        !edit.add(std::make_unique<CountSystem>(count)) ||
        !edit.commit())
    {
        return 3;
    }
    schedule.run(1.0F / 60.0F, 1U);
    return count == 2U ? 0 : 4;
}
