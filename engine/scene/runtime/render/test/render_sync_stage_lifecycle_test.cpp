#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/scene/Builtin3DRenderStages.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

int main()
{
    using namespace lux;
    using namespace lux::render;
    using namespace lux::scene;
    using namespace lux::simulation::ecs;

    constexpr std::size_t EntityCount = 10'000U;
    const std::array<TypeId, MeshStackOperationIds::kCount> mesh_ids{
        101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U, 109U, 110U, 111U, 112U
    };
    const auto mesh_ops = MeshStackOperationIds::fromOps(
        mesh_ids.data(),
        static_cast<std::uint32_t>(mesh_ids.size())
    );
    const asset::AssetId mesh_id{std::array<std::uint8_t, 16>{1U}};
    const asset::AssetId material_id{std::array<std::uint8_t, 16>{2U}};
    const RMeshHandle mesh_handle{7U, 1U};
    const RMaterialHandle material_handle{8U, 1U};

    Registry registry;

    // A discarded first-publish prepare must remove its private unpublished
    // state. A second stage can then own the same entity without an owner
    // collision; retaining a placeholder would make this prepare fail.
    {
        Registry discard_registry;
        const Entity discard_entity = discard_registry.create();
        discard_registry.emplace<Mesh3D>(
            discard_entity,
            Mesh3D{rdesc::MeshVisualDescription{mesh_id, material_id}}
        );
        discard_registry.emplace<WorldTransform3D>(discard_entity);
        discard_registry.emplace<ResolvedMeshResources>(
            discard_entity,
            ResolvedMeshResources{mesh_id, material_id, mesh_handle, material_handle}
        );
        auto first_result = createMesh3DRenderStage(Mesh3DRenderStageConfig{
            .registry = discard_registry,
            .scene = RenderSceneId{4U, 1U},
            .operations = mesh_ops
        });
        assert(first_result);
        auto first_stage = std::move(*first_result);
        RenderProgram<> discarded;
        RenderProgramBuilder<> discarded_builder{discarded};
        discarded_builder.begin();
        assert(first_stage->prepare(discarded_builder) == ERenderSyncPrepareResult::PREPARED_COMMANDS);
        first_stage->discardPrepared();
        first_stage.reset();

        auto second_result = createMesh3DRenderStage(Mesh3DRenderStageConfig{
            .registry = discard_registry,
            .scene = RenderSceneId{4U, 1U},
            .operations = mesh_ops
        });
        assert(second_result);
        RenderProgram<> retried;
        RenderProgramBuilder<> retried_builder{retried};
        retried_builder.begin();
        assert((*second_result)->prepare(retried_builder) == ERenderSyncPrepareResult::PREPARED_COMMANDS);
        (*second_result)->commitPrepared();
    }

    std::vector<Entity> entities;
    std::vector<RenderEntityId> render_entities;
    entities.reserve(EntityCount);
    render_entities.reserve(EntityCount);
    for (std::size_t index = 0U; index < EntityCount; ++index)
    {
        const Entity entity = registry.create();
        entities.push_back(entity);
        render_entities.push_back(toRenderEntity(entity));
        registry.emplace<Mesh3D>(entity, Mesh3D{rdesc::MeshVisualDescription{mesh_id, material_id}});
        registry.emplace<WorldTransform3D>(entity);
        registry.emplace<ResolvedMeshResources>(
            entity,
            ResolvedMeshResources{mesh_id, material_id, mesh_handle, material_handle}
        );
    }

    auto stage_result = createMesh3DRenderStage(Mesh3DRenderStageConfig{
        .registry = registry,
        .scene = RenderSceneId{3U, 1U},
        .operations = mesh_ops
    });
    assert(stage_result);
    auto stage = std::move(*stage_result);

    RenderProgram<> initial;
    RenderProgramBuilder<> initial_builder{initial};
    initial_builder.begin();
    assert(stage->prepare(initial_builder) == ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(initial.commands.size() == EntityCount);
    stage->commitPrepared();

    const auto destroy_started = std::chrono::steady_clock::now();
    for (const Entity entity : entities)
    {
        registry.destroy(entity);
    }
    const auto prepare_started = std::chrono::steady_clock::now();
    RenderProgram<> removal;
    RenderProgramBuilder<> removal_builder{removal};
    removal_builder.begin();
    assert(stage->prepare(removal_builder) == ERenderSyncPrepareResult::PREPARED_COMMANDS);
    const auto prepare_finished = std::chrono::steady_clock::now();
    assert(removal.commands.size() == EntityCount);

    std::ranges::sort(render_entities, [](RenderEntityId left, RenderEntityId right) {
        return static_cast<std::uint64_t>(left) < static_cast<std::uint64_t>(right);
    });
    std::vector<RenderEntityId> removed;
    removed.reserve(EntityCount);
    const CommandPacketView view{removal};
    for (const auto& command : removal.commands)
    {
        assert(command.type_id == mesh_ops.id<RemoveMeshInstanceOp>());
        const auto bytes = view.bytes(command);
        assert(bytes && bytes->size() == sizeof(RemoveMeshInstancePayload));
        removed.push_back(reinterpret_cast<const RemoveMeshInstancePayload*>(bytes->data())->entity);
    }
    std::ranges::sort(removed, [](RenderEntityId left, RenderEntityId right) {
        return static_cast<std::uint64_t>(left) < static_cast<std::uint64_t>(right);
    });
    assert(removed == render_entities);
    stage->commitPrepared();

    const auto destroy_ms = std::chrono::duration<double, std::milli>(prepare_started - destroy_started).count();
    const auto prepare_ms = std::chrono::duration<double, std::milli>(prepare_finished - prepare_started).count();
    std::printf("entities=%zu,destroy_ms=%.3f,prepare_ms=%.3f\n", EntityCount, destroy_ms, prepare_ms);
    return 0;
}
