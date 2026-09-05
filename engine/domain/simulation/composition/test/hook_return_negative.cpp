#include <lux/engine/simulation/SimulationBuilder.hpp>

namespace
{
    using namespace lux::simulation;
    struct Source final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.hook-invalid-result", .version = 1U}
        };
    };
}

void rejectMeaningfulReturn(lux::simulation::SimulationBuilder& builder) noexcept
{
    static_cast<void>(builder.addSystemHookTask<Source>(
        lux::system::SystemInstanceId{1U}, lux::simulation::HookPointId{1U},
        [](Source&, const lux::simulation::HookInvocation&) noexcept { return 42; }
    ));
}

int main() { return 0; }
