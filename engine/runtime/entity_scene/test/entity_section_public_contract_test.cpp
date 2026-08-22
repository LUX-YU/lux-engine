#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchStager.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace
{
    using lux::runtime::entity_scene::ContentBlobLease;
    using lux::runtime::entity_scene::DecodedEntityBatch;
    using lux::runtime::entity_scene::EntityBatchStager;
    using lux::runtime::entity_scene::EntitySceneCatalog;
    using lux::runtime::entity_scene::PreparedEntityBatch;

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
        const lux::scene::SceneDescription&>);

    static_assert(!std::default_initializable<EntityBatchStager>);
    static_assert(std::constructible_from<
        EntityBatchStager,
        const lux::ecs::ComponentTypeCatalog&>);
    static_assert(std::same_as<
        decltype((std::declval<
            const lux::ecs::scene_format::EntitySectionSchema&>().id.name)),
        const std::string&>);
    static_assert(std::same_as<
        decltype((std::declval<
            const lux::ecs::scene_format::EntitySectionSchema&>().id.hash)),
        const std::uint64_t&>);
}

int main()
{
    return 0;
}
