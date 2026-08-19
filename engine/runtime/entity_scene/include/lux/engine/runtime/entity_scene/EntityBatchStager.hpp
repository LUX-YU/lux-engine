#pragma once
/**
 * @file EntityBatchStager.hpp
 * @brief Budgeted LXES component materialization into a private registry.
 */

#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

namespace lux::ecs { class ComponentTypeCatalog; }

namespace lux::runtime::entity_scene
{
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntityBatchStager final
    {
    public:
        explicit EntityBatchStager(
            const lux::ecs::ComponentTypeCatalog& components) noexcept
            : components_(components)
        {}

        [[nodiscard]] lux::cxx::expected<
            PreparedEntityBatch,
            EntityBatchFailure>
        begin(
            DecodedEntityBatch decoded,
            SectionBlobStore& blobs) const noexcept;

        [[nodiscard]] lux::cxx::expected<
            EntityBatchStageResult,
            EntityBatchFailure>
        advance(
            PreparedEntityBatch& batch,
            const EntityBatchStageBudget& budget) const noexcept;

    private:
        const lux::ecs::ComponentTypeCatalog& components_;
    };
}
