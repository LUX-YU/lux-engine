#pragma once
/**
 * @file EntitySectionGeneratorCatalog.hpp
 * @brief Frozen dispatch for Engine-owned GeneratedSectionSource recipes.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime::entity_scene
{
    struct GeneratedEntitySectionRequest final
    {
        // Owning record lets a leaf generator derive identity and content from
        // one canonical source recipe. The callback must not retain views into
        // this request.
        lux::ecs::scene_format::SectionRecord record;
    };

    enum class EEntitySectionGeneratorError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_ID,
        HASH_COLLISION,
        NOT_FOUND,
        GENERATION_FAILED
    };

    struct EntitySectionGeneratorFailure final
    {
        EEntitySectionGeneratorError error{
            EEntitySectionGeneratorError::INVALID_DESCRIPTOR};
        lux::ecs::scene_format::SectionGeneratorId generator;
        std::string conflicting_name;
        std::string detail;
    };

    template <typename T>
    using EntitySectionGeneratorExp =
        lux::cxx::expected<T, EntitySectionGeneratorFailure>;

    using GenerateEntitySectionResult = EntitySectionGeneratorExp<
        lux::ecs::scene_format::EntitySectionImage>;
    using GenerateEntitySectionFn = GenerateEntitySectionResult (*)(
        const void* state,
        GeneratedEntitySectionRequest request) noexcept;

    struct EntitySectionGeneratorDescriptor final
    {
        lux::ecs::scene_format::SectionGeneratorId id;
        GenerateEntitySectionFn generate{nullptr};
        // Immutable provider state. The noexcept callback may run concurrently
        // on the background CPU arena and must treat this state as read-only.
        std::shared_ptr<const void> state;
        // Optional code-lifetime pin, separate from provider state.
        std::shared_ptr<const void> lifetime;
    };

    /// Created in one validation transaction and immutable thereafter. The
    /// loader compares full canonical IDs and invokes the selected callback; it
    /// never switches on a domain type.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC
    EntitySectionGeneratorCatalog final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const EntitySectionGeneratorCatalog>,
            EntitySectionGeneratorFailure>
        create(std::vector<EntitySectionGeneratorDescriptor> descriptors)
            noexcept;

        [[nodiscard]] GenerateEntitySectionResult generate(
            GeneratedEntitySectionRequest request) const noexcept;

        [[nodiscard]] bool contains(
            const lux::ecs::scene_format::SectionGeneratorId& generator) const noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return descriptors_.size();
        }

    private:
        explicit EntitySectionGeneratorCatalog(
            std::vector<EntitySectionGeneratorDescriptor> descriptors)
            noexcept
            : descriptors_(std::move(descriptors))
        {}

        std::vector<EntitySectionGeneratorDescriptor> descriptors_;
    };
} // namespace lux::runtime::entity_scene
