#include <lux/engine/scene/SceneCapabilityProvider.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>

#include <array>
#include <cassert>

namespace
{
    struct Contract
    {
        virtual ~Contract() noexcept = default;
    };

    struct Provider final : Contract, lux::object::LuxObject
    {
    };

    struct PlainSystem final
    {
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.test.plain",
            .version = 1U
        };
    };
}

int main()
{
    static_assert(lux::scene::SceneSystem<PlainSystem>);
    static_assert(lux::scene::sceneSystemObjectProjection<PlainSystem>() == nullptr);
    Provider provider;
    const auto value = lux::scene::makeSceneCapabilityProvider<Contract>(
        "host.test",
        "lux.test.capability",
        provider
    );
    assert(value.type == lux::cxx::typeToken<Contract>());
    assert(value.value == static_cast<Contract*>(&provider));
    assert(value.object == static_cast<lux::object::LuxObject*>(&provider));
    return 0;
}
