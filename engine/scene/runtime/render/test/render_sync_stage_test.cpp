#include <lux/engine/function/render/client/features/light/LightOperation.hpp>
#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/scene/Builtin3DRenderStages.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    lux::render::RenderProgram<> prepare(
        lux::scene::RenderSyncStage& stage,
        lux::scene::ERenderSyncPrepareResult expected
    )
    {
        lux::render::RenderProgram<> program;
        lux::render::RenderProgramBuilder<> builder{program};
        builder.begin();
        program.kind = lux::render::ERenderProgramKind::StateUpdate;
        assert(stage.prepare(builder) == expected);
        assert(builder.valid());
        return program;
    }

    template <class Payload>
    const Payload& payload(const lux::render::RenderProgram<>& program, std::size_t command = 0U)
    {
        const lux::render::CommandPacketView view{program};
        const auto bytes = view.bytes(program.commands[command]);
        assert(bytes && bytes->size() >= sizeof(Payload));
        return *reinterpret_cast<const Payload*>(bytes->data());
    }
}

int main()
{
    using namespace lux;
    using namespace lux::render;
    using namespace lux::scene;
    using namespace lux::simulation::ecs;

    const std::array<TypeId, MeshStackOperationIds::kCount> mesh_ids{
        101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U, 109U, 110U, 111U, 112U
    };
    const std::array<TypeId, LightOperationIds::kCount> light_ids{201U, 202U, 203U, 204U};
    const auto mesh_ops = MeshStackOperationIds::fromOps(mesh_ids.data(), static_cast<std::uint32_t>(mesh_ids.size()));
    const auto light_ops = LightOperationIds::fromOps(light_ids.data(), static_cast<std::uint32_t>(light_ids.size()));
    const RenderSceneId scene_id{3U, 1U};

    Registry mesh_registry;
    const asset::AssetId mesh_a{std::array<std::uint8_t, 16>{1U}};
    const asset::AssetId material_a{std::array<std::uint8_t, 16>{2U}};
    const asset::AssetId mesh_b{std::array<std::uint8_t, 16>{3U}};
    const asset::AssetId material_b{std::array<std::uint8_t, 16>{4U}};
    const RMeshHandle mesh_handle_a{7U, 1U};
    const RMaterialHandle material_handle_a{8U, 1U};
    const RMeshHandle mesh_handle_b{9U, 2U};
    const RMaterialHandle material_handle_b{10U, 2U};

    const Entity mesh_entity = mesh_registry.create();
    mesh_registry.emplace<Mesh3D>(mesh_entity, Mesh3D{rdesc::MeshVisualDescription{mesh_a, material_a}});
    mesh_registry.emplace<WorldTransform3D>(mesh_entity);
    auto mesh_stage_result = createMesh3DRenderStage(Mesh3DRenderStageConfig{
        .registry = &mesh_registry,
        .scene = scene_id,
        .operations = mesh_ops
    });
    assert(mesh_stage_result);
    auto mesh_stage = std::move(*mesh_stage_result);

    auto program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_NO_COMMANDS);
    assert(program.commands.empty());
    mesh_stage->commitPrepared();

    mesh_registry.emplace<ResolvedMeshResources>(
        mesh_entity,
        ResolvedMeshResources{mesh_a, material_a, mesh_handle_a, material_handle_a}
    );
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == mesh_ops.id<UpsertMeshInstanceOp>());
    const auto& first_upsert = payload<UpsertMeshInstancePayload>(program);
    assert(first_upsert.mesh == mesh_handle_a && first_upsert.material == material_handle_a);
    mesh_stage->commitPrepared();

    for (std::size_t patch = 0U; patch < 1000U; ++patch)
    {
        mesh_registry.patch<WorldTransform3D>(mesh_entity, [](auto& world) { world.value.translation().x() += 0.01; });
    }
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == mesh_ops.id<TransformBatchOp>());
    assert(program.commands[0].payload_size == sizeof(TransformWriteEntry));
    mesh_stage->commitPrepared();

    mesh_registry.patch<Mesh3D>(mesh_entity, [](auto& mesh) { mesh.value.visible = false; });
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == mesh_ops.id<UpdateInstanceFlagsOp>());
    mesh_stage->commitPrepared();

    mesh_registry.patch<Mesh3D>(mesh_entity, [](auto& mesh) { mesh.value.visible = true; });
    mesh_registry.patch<WorldTransform3D>(mesh_entity, [](auto& world) { world.value.translation().y() += 2.0; });
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 2U);
    assert(program.commands[0].type_id == mesh_ops.id<UpdateInstanceFlagsOp>());
    assert(program.commands[1].type_id == mesh_ops.id<TransformBatchOp>());
    mesh_stage->commitPrepared();

    mesh_registry.patch<Mesh3D>(mesh_entity, [](auto&) {});
    mesh_registry.patch<WorldTransform3D>(mesh_entity, [](auto&) {});
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_NO_COMMANDS);
    assert(program.commands.empty());
    mesh_stage->commitPrepared();

    mesh_registry.patch<Mesh3D>(mesh_entity, [mesh_b, material_b](auto& mesh) {
        mesh.value.mesh = mesh_b;
        mesh.value.material = material_b;
    });
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_NO_COMMANDS);
    assert(program.commands.empty());
    mesh_stage->commitPrepared();

    mesh_registry.patch<ResolvedMeshResources>(mesh_entity, [&](auto& resolved) {
        resolved = ResolvedMeshResources{mesh_b, material_b, mesh_handle_b, material_handle_b};
    });
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == mesh_ops.id<UpsertMeshInstanceOp>());
    mesh_stage->commitPrepared();

    mesh_registry.remove<ResolvedMeshResources>(mesh_entity);
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == mesh_ops.id<RemoveMeshInstanceOp>());
    mesh_stage->commitPrepared();

    const Entity transient = mesh_registry.create();
    mesh_registry.emplace<Mesh3D>(transient, Mesh3D{rdesc::MeshVisualDescription{mesh_a, material_a}});
    mesh_registry.emplace<WorldTransform3D>(transient);
    mesh_registry.emplace<ResolvedMeshResources>(
        transient,
        ResolvedMeshResources{mesh_a, material_a, mesh_handle_a, material_handle_a}
    );
    mesh_registry.destroy(transient);
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_NO_COMMANDS);
    assert(program.commands.empty());
    mesh_stage->commitPrepared();

    mesh_registry.emplace<ResolvedMeshResources>(
        mesh_entity,
        ResolvedMeshResources{mesh_b, material_b, mesh_handle_b, material_handle_b}
    );
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    mesh_stage->commitPrepared();
    const auto old_render_entity = toRenderEntity(mesh_entity);
    mesh_registry.destroy(mesh_entity);
    const Entity replacement = mesh_registry.create();
    mesh_registry.emplace<Mesh3D>(replacement, Mesh3D{rdesc::MeshVisualDescription{mesh_a, material_a}});
    mesh_registry.emplace<WorldTransform3D>(replacement);
    mesh_registry.emplace<ResolvedMeshResources>(
        replacement,
        ResolvedMeshResources{mesh_a, material_a, mesh_handle_a, material_handle_a}
    );
    program = prepare(*mesh_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 2U);
    assert(program.commands[0].type_id == mesh_ops.id<RemoveMeshInstanceOp>());
    assert(program.commands[1].type_id == mesh_ops.id<UpsertMeshInstanceOp>());
    assert(payload<RemoveMeshInstancePayload>(program, 0U).entity == old_render_entity);
    assert(payload<UpsertMeshInstancePayload>(program, 1U).entity == toRenderEntity(replacement));
    mesh_stage->commitPrepared();

    Registry light_registry;
    std::array<Entity, 4> lights{};
    for (std::size_t index = 0U; index < lights.size(); ++index)
    {
        lights[index] = light_registry.create();
        Light3D light{};
        light.value.type = static_cast<rdesc::ELightType>(index);
        light.value.cast_shadow = index == 0U;
        light_registry.emplace<Light3D>(lights[index], light);
        light_registry.emplace<WorldTransform3D>(lights[index]);
    }
    auto light_stage_result = createLight3DRenderStage(Light3DRenderStageConfig{
        .registry = &light_registry,
        .scene = scene_id,
        .operations = light_ops
    });
    assert(light_stage_result);
    auto light_stage = std::move(*light_stage_result);
    program = prepare(*light_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == light_ops.id<LightBatchOp>());
    const CommandPacketView light_view{program};
    const auto light_bytes = light_view.bytes(program.commands[0]);
    assert(light_bytes && light_bytes->size() == sizeof(UpsertLightPayload) * lights.size());
    const auto* light_payloads = reinterpret_cast<const UpsertLightPayload*>(light_bytes->data());
    std::vector<std::uint8_t> light_types;
    for (std::size_t index = 0U; index < lights.size(); ++index)
    {
        light_types.push_back(light_payloads[index].light_type);
        if (light_payloads[index].light_type == 0U)
        {
            assert((light_payloads[index].flags & LIGHT_FLAG_CAST_SHADOW) != 0U);
            assert(std::abs(light_payloads[index].direction[0]) < 1.0e-6F);
            assert(std::abs(light_payloads[index].direction[1] + 1.0F) < 1.0e-6F);
            assert(std::abs(light_payloads[index].direction[2]) < 1.0e-6F);
        }
    }
    std::ranges::sort(light_types);
    assert((light_types == std::vector<std::uint8_t>{0U, 1U, 2U, 3U}));
    light_stage->commitPrepared();

    for (std::size_t patch = 0U; patch < 1000U; ++patch)
    {
        light_registry.patch<WorldTransform3D>(lights[1], [](auto& world) { world.value.translation().z() += 0.01; });
    }
    program = prepare(*light_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == light_ops.id<LightBatchOp>());
    light_stage->commitPrepared();

    light_registry.remove<Light3D>(lights[1]);
    program = prepare(*light_stage, ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(program.commands.size() == 1U);
    assert(program.commands[0].type_id == light_ops.id<RemoveLightOp>());
    light_stage->commitPrepared();
    return 0;
}
