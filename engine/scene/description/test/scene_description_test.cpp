#include <lux/engine/scene/SceneDescriptionBuilder.hpp>

#include <array>
#include <cassert>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = value;
        return lux::asset::AssetId(bytes);
    }
}

int main()
{
    using namespace lux;

    scene::SceneDescriptionBuilder builder;
    builder.setWorld(id(1U));
    builder.setSimulation(id(2U));
    const auto first_type = system::systemTypeId("lux.scene.test.first");
    const auto second_type = system::systemTypeId("lux.scene.test.second");
    constexpr system::SystemInstanceId First{20U};
    constexpr system::SystemInstanceId Second{10U};
    const std::array configuration{std::byte{1U}, std::byte{2U}};
    assert(builder.addSystem(First, "first", first_type, 1U, "lux.scene.test.Config", 1U, configuration));
    assert(builder.addSystem(Second, "second", second_type, 2U, {}, 0U));
    assert(builder.bindRequirement(First, "runtime", "host.primary"));
    assert(builder.addDependency(Second, First));

    auto description = std::move(builder).build();
    assert(description);
    assert(description->world() == id(1U) && description->simulation() == id(2U));
    assert(description->systemCount() == 2U);
    assert(description->systemAt(0U).instanceId() == Second);
    assert(description->systemAt(1U).instanceId() == First);
    assert(description->findSystem("first").configurationPayload().size() == 2U);
    assert(description->findSystem(First).findRequirementBinding("runtime").provider() == "host.primary");
    assert(description->dependencyAt(0U).before() == Second);

    scene::SceneDescriptionBuilder cycle;
    cycle.setWorld(id(1U));
    cycle.setSimulation(id(2U));
    assert(cycle.addSystem(First, "first", first_type, 1U, {}, 0U));
    assert(cycle.addSystem(Second, "second", second_type, 1U, {}, 0U));
    assert(cycle.addDependency(First, Second));
    assert(cycle.addDependency(Second, First));
    const auto cycle_result = std::move(cycle).build();
    assert(!cycle_result && cycle_result.error().code == scene::ESceneDescriptionError::DEPENDENCY_CYCLE);
    return 0;
}
