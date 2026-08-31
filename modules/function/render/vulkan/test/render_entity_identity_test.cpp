#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace
{
    struct SceneEntityIdentity final
    {
        lux::render::RenderSceneId scene{};
        lux::render::RenderEntityId entity{};

        friend bool operator==(const SceneEntityIdentity&, const SceneEntityIdentity&) noexcept = default;
    };
}

int main()
{
    using lux::render::RenderEntityId;
    static_assert(sizeof(RenderEntityId) == sizeof(std::uint64_t));
    static_assert(std::is_enum_v<RenderEntityId>);

    constexpr std::uint64_t bits = 0xfedc'ba98'7654'3210ULL;
    static_assert(static_cast<std::uint64_t>(static_cast<RenderEntityId>(bits)) == bits);

    lux::render::RenderScene::EntityRegistry entities;
    const RenderEntityId first = entities.create();
    const SceneEntityIdentity first_identity{{7, 3}, first};
    assert(entities.valid(first));
    entities.destroy(first);

    const RenderEntityId replacement = entities.create();
    const SceneEntityIdentity replacement_identity{{7, 3}, replacement};
    assert(entities.valid(replacement));
    assert(first != replacement);
    assert(first_identity != replacement_identity);

    static_assert(std::is_same_v<
                  decltype(std::declval<lux::render::RenderScene&>().entities()),
                  lux::render::RenderScene::EntityRegistry&>);
    static_assert(std::is_same_v<
                  decltype(std::declval<lux::render::RenderScene&>().resources()),
                  lux::render::ResourceRegistry&>);
    return 0;
}
