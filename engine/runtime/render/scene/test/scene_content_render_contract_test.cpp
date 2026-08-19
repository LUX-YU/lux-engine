#include <lux/engine/runtime/render/scene/detail/SceneContentRenderContracts.hpp>

#include <lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp>
#include <lux/engine/function/render/client/features/render_cluster/RenderClusterOperation.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/resource/classic_mesh/ClassicMeshBatch.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace
{
    [[nodiscard]] uuids::uuid makeUuid(std::uint8_t marker)
    {
        std::array<std::uint8_t, 16u> bytes{};
        bytes[0] = marker;
        return uuids::uuid{bytes};
    }
}

int main()
{
    using namespace lux::runtime::detail;

    ContentRenderRevisionSequence revisions;
    const auto first = revisions.next();
    const auto patch = revisions.next();
    const auto remove = revisions.next();
    if (first != 1u || patch != 2u || remove != 3u ||
        !(first < patch && patch < remove))
        return 1;

    if (classifyContentUploadReply(true, true, false, 0u) !=
            EContentUploadReplyDisposition::COMMIT ||
        classifyContentUploadReply(true, true, false, 3u) !=
            EContentUploadReplyDisposition::FAIL_DOMAIN ||
        classifyContentUploadReply(false, false, false, 0u) !=
            EContentUploadReplyDisposition::COMPENSATE_REMOVE ||
        classifyContentUploadReply(false, false, true, 0u) !=
            EContentUploadReplyDisposition::FAIL_DOMAIN)
        return 2;

    if (!validVisualLodContract(4.0f, 2.5f, 1.5f) ||
        validVisualLodContract(4.0f, 1.5f, 2.5f) ||
        !validTerrainLodContract(0u, 0u, 0.0f, 2.5f, 1.5f) ||
        !validTerrainLodContract(1u, 4u, 8.0f, 2.5f, 1.5f) ||
        validTerrainLodContract(1u, 0u, 8.0f, 2.5f, 1.5f))
        return 3;

    if (classifyContentPreparation(true, true, true) !=
            EContentPreparationDisposition::COMMIT ||
        classifyContentPreparation(true, true, false) !=
            EContentPreparationDisposition::RETRY_LATEST ||
        classifyContentPreparation(false, false, false) !=
            EContentPreparationDisposition::DISCARD_STALE ||
        classifyContentPreparation(true, false, true) !=
            EContentPreparationDisposition::DISCARD_STALE)
    {
        return 8;
    }

    ContentRenderOwnerSequence owner_generations;
    const auto retired_owner = owner_generations.next();
    const auto replacement_owner = owner_generations.next();
    if (retired_owner == 0u || replacement_owner == 0u ||
        retired_owner == replacement_owner ||
        classifyContentPreparation(
            retired_owner == replacement_owner,
            true,
            true) != EContentPreparationDisposition::DISCARD_STALE)
    {
        return 9;
    }

    constexpr std::size_t kInstanceCount = 100'000u;
    lux::classic_mesh::ClassicMeshBatchBlobV1 source;
    source.instances.resize(kInstanceCount);
    const auto mesh = makeUuid(0x31u);
    for (std::size_t index = 0u; index != source.instances.size(); ++index)
    {
        auto& row = source.instances[index];
        row.mesh_asset = mesh;
        row.translation[0] = static_cast<float>(index % 1000u);
        row.translation[2] = static_cast<float>(index / 1000u);
        row.stable_pick_id = index + 1u;
    }
    auto encoded = lux::classic_mesh::encodeClassicMeshBatchBlob(source);
    if (!encoded)
        return 4;
    auto decoded = lux::classic_mesh::decodeClassicMeshBatchBlob(*encoded);
    if (!decoded || decoded->instances.size() != kInstanceCount)
        return 5;

    lux::ecs::scene_format::ContentBlobRef content;
    content.id.digest[0] = std::byte{0x42u};
    content.type = lux::ecs::scene_format::ContentTypeId{std::string{
        lux::classic_mesh::kClassicMeshBatchContentTypeName}};
    content.schema_version =
        lux::classic_mesh::kClassicMeshBatchSchemaVersion;
    lux::meta::EntityRegistry registry;
    const auto entity = registry.create();
    registry.emplace<lux::ecs::ClassicMeshBatchComponent>(
        entity,
        lux::ecs::ClassicMeshBatchComponent{
            .content = std::move(content),
            .local_bounds_center = Eigen::Vector3f::Zero(),
            .local_bounds_radius = 2048.0f});
    registry.patch<lux::ecs::ClassicMeshBatchComponent>(
        entity,
        [](lux::ecs::ClassicMeshBatchComponent& batch)
        {
            batch.local_bounds_radius = 1024.0f;
        });
    const auto& patched = registry.get<const
        lux::ecs::ClassicMeshBatchComponent>(entity);
    if (registry.view<lux::ecs::ClassicMeshBatchComponent>().size() !=
            1u ||
        decoded->instances.size() != kInstanceCount ||
        patched.local_bounds_radius != 1024.0f)
        return 6;

    lux::render::UploadRenderClusterPayload operation{};
    operation.revision = patch;
    operation.instance_count = static_cast<std::uint32_t>(
        decoded->instances.size());
    operation.lod_level = 1u;
    operation.child_count = 4u;
    if (operation.revision != 2u || operation.instance_count != 100'000u ||
        operation.child_count != 4u)
        return 7;
    return 0;
}
