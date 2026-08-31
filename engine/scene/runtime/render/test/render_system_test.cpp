#include <lux/engine/scene/RenderSystem.hpp>

#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

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
    auto mesh_ops = MeshStackOperationIds::fromOps(mesh_ids.data(), static_cast<std::uint32_t>(mesh_ids.size()));
    auto light_ops = LightOperationIds::fromOps(light_ids.data(), static_cast<std::uint32_t>(light_ids.size()));

    Registry registry;
    const Entity entity = registry.create();
    const asset::AssetId mesh_id{std::array<std::uint8_t, 16>{1U}};
    const asset::AssetId material_id{std::array<std::uint8_t, 16>{2U}};
    registry.emplace<Mesh3D>(entity, Mesh3D{mesh_id, material_id});
    WorldTransform3D world{};
    world.value.translation() = Eigen::Vector3d{1.0e12 + 0.125, 2.0, -3.0};
    registry.emplace<WorldTransform3D>(entity, world);

    auto created = RenderSystem::create(
        registry,
        RenderSystem::Config{
            .scene = {3U, 1U},
            .mesh_stack = mesh_ops,
            .light = light_ops,
            .coordinate_page_size = 1024.0,
            .scene_origin_page = {976562500, 0, 0},
            .expected_entity_capacity = 64U
        }
    );
    assert(created);
    auto system = std::move(*created);
    assert(system->tryPublish() == ERenderPublishResult::FullSyncPublished);

    registry.patch<WorldTransform3D>(entity, [](auto& transform) { transform.value.translation().x() += 4.0; });
    assert(system->tryPublish() == ERenderPublishResult::Backpressured);

    auto channel = RenderProgramChannel<>::create(1U);
    auto sync = std::make_shared<RenderChannelSync>();
    RenderProgramSession session{channel, sync};
    assert(system->tryForwardUpdate(session) == ERenderForwardResult::Forwarded);
    assert(channel->requests.tryAcquireRead());
    const auto& first = channel->requests.currentRead();
    assert(first.kind == ERenderProgramKind::StateUpdate);
    assert(!first.commands.empty());
    assert(first.commands.front().type_id == mesh_ids[0]);

    assert(system->tryPublish() == ERenderPublishResult::Published);
    assert(system->tryForwardUpdate(session) == ERenderForwardResult::Forwarded);
    assert(channel->requests.tryAcquireRead());

    registry.destroy(entity);
    const Entity replacement = registry.create();
    assert(entt::to_entity(replacement) == entt::to_entity(entity));
    assert(replacement != entity);
    registry.emplace<Mesh3D>(replacement, Mesh3D{mesh_id, material_id});
    registry.emplace<WorldTransform3D>(replacement, world);
    assert(system->tryPublish() == ERenderPublishResult::Published);
    assert(system->tryForwardUpdate(session) == ERenderForwardResult::Forwarded);
    assert(channel->requests.tryAcquireRead());
    const auto& reused = channel->requests.currentRead();
    assert(reused.commands.size() >= 2U);
    assert(reused.commands[0].type_id == mesh_ids[1]);
    assert(reused.commands[1].type_id == mesh_ids[0]);
    const CommandPacketView reused_view{reused};
    const auto remove_bytes = reused_view.bytes(reused.commands[0]);
    const auto upsert_bytes = reused_view.bytes(reused.commands[1]);
    assert(remove_bytes && upsert_bytes);
    const auto* remove = reinterpret_cast<const RemoveMeshInstancePayload*>(remove_bytes->data());
    const auto* upsert = reinterpret_cast<const UpsertMeshInstancePayload*>(upsert_bytes->data());
    assert(remove->entity == toRenderEntity(entity));
    assert(upsert->entity == toRenderEntity(replacement));

    const auto schemas = visualComponentSchemas();
    assert(schemas.size() == 2U);
    assert(schemas[0].decode_emplace != nullptr);
    assert(schemas[1].decode_emplace != nullptr);
    return 0;
}
