#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/object/Object.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    class ObjectSystem final
        : public lux::object::Object<ObjectSystem>,
          public lux::ecs::System
    {
      public:
        explicit ObjectSystem(std::uint32_t& count) noexcept : count_(&count) {}

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
    lux::ecs::Schedule schedule{world};
    std::uint32_t count{};
    auto edit_result = schedule.edit();
    if (!edit_result)
        return 1;
    auto edit = std::move(*edit_result);
    if (!edit.add(std::make_unique<ObjectSystem>(count)) || !edit.commit())
        return 2;
    schedule.run(1.0F / 60.0F, 1U);
    return count == 1U ? 0 : 3;
}
