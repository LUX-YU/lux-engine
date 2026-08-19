#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

namespace
{
    using lux::runtime::entity_scene::DecodedEntityBatch;
    using lux::runtime::entity_scene::EntitySceneCatalog;
    using lux::runtime::entity_scene::PreparedEntityBatch;
    using lux::runtime::entity_scene::ContentBlobLease;

    static_assert(std::same_as<
        decltype(std::declval<const DecodedEntityBatch&>().section()),
        const lux::ecs::scene_format::EntitySectionId&>);
    static_assert(std::same_as<
        decltype(std::declval<const PreparedEntityBatch&>().section()),
        const lux::ecs::scene_format::EntitySectionId&>);
    static_assert(std::same_as<
        decltype(std::declval<const ContentBlobLease&>().reference()),
        const lux::ecs::scene_format::ContentBlobRef&>);
    static_assert(std::same_as<
        decltype(std::declval<const EntitySceneCatalog&>().package()),
        const lux::scene::ScenePackage&>);
}

int main()
{
    return 0;
}
