#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>

#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace lux::runtime::entity_scene
{
    lux::cxx::expected<
        std::shared_ptr<const EntitySectionGeneratorCatalog>,
        EntitySectionGeneratorFailure>
    EntitySectionGeneratorCatalog::create(
        std::vector<EntitySectionGeneratorDescriptor> descriptors) noexcept
    {
        for (const auto& descriptor : descriptors)
        {
            if (!lux::ecs::scene_format::isValidSectionGeneratorId(descriptor.id) ||
                !descriptor.generate)
            {
                return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                    EEntitySectionGeneratorError::INVALID_DESCRIPTOR,
                    descriptor.id,
                    {},
                    "invalid EntitySection generator descriptor"});
            }
        }
        std::sort(
            descriptors.begin(),
            descriptors.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.id.name() < rhs.id.name();
            });
        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            if (index != 0u &&
                descriptors[index - 1u].id.name() ==
                    descriptors[index].id.name())
            {
                return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                    EEntitySectionGeneratorError::DUPLICATE_ID,
                    descriptors[index].id,
                    std::string{descriptors[index - 1u].id.name()},
                    "duplicate EntitySection generator ID"});
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                if (descriptors[other].id.hash() ==
                    descriptors[index].id.hash())
                {
                    return lux::cxx::unexpected(
                        EntitySectionGeneratorFailure{
                            EEntitySectionGeneratorError::HASH_COLLISION,
                            descriptors[index].id,
                            std::string{descriptors[other].id.name()},
                            "EntitySection generator hash collision"});
                }
            }
        }
        return std::shared_ptr<const EntitySectionGeneratorCatalog>{
            new EntitySectionGeneratorCatalog{std::move(descriptors)}};
    }

    GenerateEntitySectionResult EntitySectionGeneratorCatalog::generate(
        GeneratedEntitySectionRequest request) const noexcept
    {
        const auto valid = lux::scene::validateSectionRecord(request.record);
        const auto* source =
            std::get_if<lux::ecs::scene_format::GeneratedSectionSource>(
                &request.record.source);
        if (!valid || !source)
        {
            return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                EEntitySectionGeneratorError::GENERATION_FAILED,
                source ? source->generator
                       : lux::ecs::scene_format::SectionGeneratorId{},
                {},
                "invalid generated EntitySection request"});
        }

        const auto& generator = source->generator;
        const auto found = std::lower_bound(
            descriptors_.begin(),
            descriptors_.end(),
            generator.name(),
            [](const auto& descriptor, std::string_view name)
            {
                return descriptor.id.name() < name;
            });
        if (found == descriptors_.end() ||
            found->id.view() != generator.view())
        {
            return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                EEntitySectionGeneratorError::NOT_FOUND,
                generator,
                {},
                "EntitySection generator is not registered"});
        }
        return found->generate(found->state.get(), std::move(request));
    }

    bool EntitySectionGeneratorCatalog::contains(
        const lux::ecs::scene_format::SectionGeneratorId& generator) const noexcept
    {
        if (!lux::ecs::scene_format::isValidSectionGeneratorId(generator))
            return false;
        const auto found = std::lower_bound(
            descriptors_.begin(),
            descriptors_.end(),
            generator.name(),
            [](const auto& descriptor, std::string_view name)
            {
                return descriptor.id.name() < name;
            });
        return found != descriptors_.end() &&
            found->id.view() == generator.view();
    }
} // namespace lux::runtime::entity_scene
