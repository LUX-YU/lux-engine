#pragma once
/**
 * @file EntityBatchMaterializer.hpp
 * @brief Failure-free command-barrier publication of prepared EntitySections.
 */

#include <lux/engine/runtime/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::meta { class EntityRegistry; }
namespace lux::ecs { class PersistentEntityIndex; }

namespace lux::runtime::entity_scene
{
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC SectionCommitReceipt final
    {
    public:
        ~SectionCommitReceipt();
        SectionCommitReceipt(const SectionCommitReceipt&) = delete;
        SectionCommitReceipt& operator=(const SectionCommitReceipt&) = delete;

        [[nodiscard]] const lux::entity_scene::EntitySectionId& section()
            const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::span<const entt::entity> entities() const noexcept;

    private:
        friend class EntityBatchMaterializer;
        SectionCommitReceipt();
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntityBatchMaterializer final
    {
    public:
        /// Borrows the one sparse identity authority for the live registry.
        /// arm() rejects a registry which is not bound to that authority.
        explicit EntityBatchMaterializer(lux::ecs::PersistentEntityIndex& persistent_entities);
        ~EntityBatchMaterializer();
        EntityBatchMaterializer(const EntityBatchMaterializer&) = delete;
        EntityBatchMaterializer& operator=(
            const EntityBatchMaterializer&) = delete;

        // arm() excludes ordinary input/registry errors and preflights the
        // known packed/payload capacities, including every earlier batch
        // still ARMED for the same barrier. It also arms an isolated
        // registry-owned sparse-page reservation for this batch. A successful
        // result makes publishAtBarrier() free of recoverable content errors
        // and forbids registry upstream growth until its atomic publication
        // has completed.
        [[nodiscard]] lux::cxx::expected<void, EntityBatchFailure> arm(
            PreparedEntityBatch& batch,
            lux::meta::EntityRegistry& live) noexcept;

        [[nodiscard]] const SectionCommitReceipt& publishAtBarrier(
            PreparedEntityBatch& batch,
            lux::meta::EntityRegistry& live) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EntityBatchFailure>
        cancelArmed(
            PreparedEntityBatch& batch,
            lux::meta::EntityRegistry& live) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EntityBatchFailure> deactivate(
            const lux::entity_scene::EntitySectionId& section,
            std::uint64_t generation,
            lux::meta::EntityRegistry& live) noexcept;

        [[nodiscard]] const SectionCommitReceipt* find(
            const lux::entity_scene::EntitySectionId& section) const noexcept;
        [[nodiscard]] EntityBatchMaterializerSnapshot snapshot() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
