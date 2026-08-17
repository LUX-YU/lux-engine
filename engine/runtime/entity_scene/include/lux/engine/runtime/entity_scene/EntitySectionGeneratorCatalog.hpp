#pragma once
/**
 * @file EntitySectionGeneratorCatalog.hpp
 * @brief Frozen, domain-neutral dispatch for GeneratedSectionSource.
 */

#include <lux/engine/resource/entity_scene/EntityScene.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

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
        // Owning record lets a leaf generator derive identity and content
        // from the same canonical source descriptor validated by the loader.
        // The callback must not retain views into this request.
        lux::entity_scene::EntitySectionRecord record;
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
        lux::entity_scene::SectionGeneratorId generator;
        std::string conflicting_name;
        std::string detail;
    };

    using GenerateEntitySectionResult =
        lux::cxx::expected<lux::entity_scene::EntitySectionImage, EntitySectionGeneratorFailure>;
    using GenerateEntitySectionFn = GenerateEntitySectionResult (*)(
        const void* state,
        GeneratedEntitySectionRequest request) noexcept;

    struct EntitySectionGeneratorDescriptor final
    {
        lux::entity_scene::SectionGeneratorId id;
        GenerateEntitySectionFn generate{nullptr};
        // Immutable provider state. The noexcept callback may run concurrently
        // on the background CPU arena and must treat this state as read-only.
        std::shared_ptr<const void> state;
        // Optional module/code lifetime pin, separate from provider state.
        std::shared_ptr<const void> lifetime;
    };

    /// Created in one validation transaction and immutable thereafter. A leaf
    /// pack adds a generator by supplying one descriptor to create(); the
    /// EntitySection service only compares the full canonical ID and invokes
    /// the selected callback. It never switches on a domain type.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC
    EntitySectionGeneratorCatalog final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<const EntitySectionGeneratorCatalog>,
            EntitySectionGeneratorFailure>
        create(std::vector<EntitySectionGeneratorDescriptor> descriptors)
            noexcept;

        [[nodiscard]] lux::cxx::expected<
            lux::entity_scene::EntitySectionImage,
            EntitySectionGeneratorFailure>
        generate(
            GeneratedEntitySectionRequest request) const noexcept;

        [[nodiscard]] bool contains(
            const lux::entity_scene::SectionGeneratorId& generator) const
            noexcept;

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
}
