#include <lux/engine/ecs/System.hpp>

class retired_system_extrinsic_negative final : public lux::ecs::System
{
  public:
    void update(lux::ecs::SystemFrame&) noexcept override {}

    bool removable() const noexcept override
    {
        return true;
    }
};

int main()
{
}
