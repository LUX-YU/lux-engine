#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneSystem.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = value;
        return lux::asset::AssetId(bytes);
    }

    struct ProbeSystem final
    {
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "lux.consumer.scene-system",
            .version = 1U
        };
    };
}

int main()
{
    static_assert(lux::scene::SceneSystem<ProbeSystem>);
    lux::scene::SceneDescriptionBuilder builder;
    builder.setWorld(id(1U));
    builder.setSimulation(id(2U));
    const auto type = lux::system::systemTypeId(ProbeSystem::Description.canonical_name);
    if (!builder.addSystem({1U}, "probe", type, 1U, {}, 0U))
    {
        return 1;
    }
    auto description = std::move(builder).build();
    return description && description->findSystem("probe") ? 0 : 2;
}
