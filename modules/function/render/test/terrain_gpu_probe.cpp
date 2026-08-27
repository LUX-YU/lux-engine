#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    void writeU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] std::uint16_t readU16(const std::vector<std::byte>& bytes, std::size_t offset)
    {
        std::uint16_t value{0u};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> makeTerrainPage()
    {
        using namespace lux::render;
        auto bytes = std::make_shared<std::vector<std::byte>>(TerrainResources::expectedPageBytes(), std::byte{});
        TerrainWirePageDataHeader header;
        header.height_count = kTerrainWireSampleEdge * kTerrainWireSampleEdge;
        header.weight_plane_bytes = header.height_count * 4u;
        header.hole_bytes = (header.height_count + 7u) / 8u;
        header.min_max_node_count = kTerrainWireMinMaxNodeCount;
        constexpr auto fallback_edge = (kTerrainWireQuadEdge / 2u) + 1u;
        header.fallback_height_count = fallback_edge * fallback_edge;
        std::memcpy(bytes->data(), &header, sizeof(header));

        auto offset = sizeof(header);
        const auto height_offset = offset;
        for (std::uint32_t y = 0u; y < kTerrainWireSampleEdge; ++y)
        {
            for (std::uint32_t x = 0u; x < kTerrainWireSampleEdge; ++x)
            {
                writeU16(
                    *bytes,
                    height_offset + static_cast<std::size_t>(y * kTerrainWireSampleEdge + x) * sizeof(std::uint16_t),
                    static_cast<std::uint16_t>(30000u + (x + y) % 4096u)
                );
            }
        }
        offset += static_cast<std::size_t>(header.height_count) * sizeof(std::uint16_t);
        for (std::uint32_t sample = 0u; sample < header.height_count; ++sample)
        {
            (*bytes)[offset + sample * 4u] = std::byte{0xffu};
        }
        offset += static_cast<std::size_t>(header.weight_plane_bytes) * 2u;
        offset += header.hole_bytes;
        const auto min_max_offset = offset;
        const auto root_offset =
            min_max_offset + static_cast<std::size_t>(header.min_max_node_count - 1u) * sizeof(std::uint16_t) * 2u;
        writeU16(*bytes, root_offset, 30000u);
        writeU16(*bytes, root_offset + sizeof(std::uint16_t), 30512u);
        offset += static_cast<std::size_t>(header.min_max_node_count) * sizeof(std::uint16_t) * 2u;
        for (std::uint32_t y = 0u; y < fallback_edge; ++y)
        {
            for (std::uint32_t x = 0u; x < fallback_edge; ++x)
            {
                const auto source_index = (y * 2u) * kTerrainWireSampleEdge + x * 2u;
                writeU16(
                    *bytes,
                    offset + static_cast<std::size_t>(y * fallback_edge + x) * sizeof(std::uint16_t),
                    readU16(*bytes, height_offset + static_cast<std::size_t>(source_index) * sizeof(std::uint16_t))
                );
            }
        }
        return bytes;
    }

    [[nodiscard]] lux::render::RenderRequest<lux::render::TerrainPageUploadedReply> submitTerrain(
        const lux::render::RenderUploadClient& client,
        lux::render::TerrainOperationIds operations,
        lux::render::UploadTerrainPagePayload payload,
        std::shared_ptr<const std::vector<std::byte>> bytes
    )
    {
        const auto size = bytes->size();
        auto submitted = client.trySubmit<lux::render::TerrainPageUploadedReply>(
            [bytes, payload, operation = operations.id<lux::render::TerrainPageUploadOp>()](
                lux::render::RenderUploadClient::Builder& builder) mutable {
                payload.page_data = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(bytes),
                    bytes->data(),
                    static_cast<std::uint32_t>(bytes->size()),
                    lux::render::attachment_types::OwnedBytes,
                    bytes->size()
                );
                builder.pushPreparedResource(operation, payload);
            },
            lux::render::UploadPayloadAccounting{.shared_bytes = size}
        );
        return submitted ? std::move(*submitted) : lux::render::RenderRequest<lux::render::TerrainPageUploadedReply>{};
    }
} // namespace

int
main()
{
    using namespace lux::render;
    std::atomic<int> validation_errors{0};
    {
        lux::rendertest::DeviceRenderFixture fixture(
            32u,
            32u,
            "terrain_gpu_probe",
            {.enable_validation = true, .validation_errors = &validation_errors}
        );
        if (!fixture.ok())
        {
            std::printf("SKIP: Vulkan device unavailable\n");
            return 0;
        }
        const auto scene = fixture.makeSceneWithView("TerrainProbe", "TerrainProbeView");

        const auto material_type = fixture.awaitControl(fixture.control().registerFeatureType(kMaterialFeatureFactory));
        if (!fixture
                 .awaitControl(
                     fixture.control().addFeature(scene.scene_id, material_type.feature_type_id, MaterialCommTag{})
                 )
                 .feature.isValid())
        {
            return 1;
        }
        const auto mesh_type = fixture.awaitControl(fixture.control().registerFeatureType(kMeshStackFeatureFactory));
        if (!fixture
                 .awaitControl(
                     fixture.control().addFeature(scene.scene_id, mesh_type.feature_type_id, MeshStackCommTag{})
                 )
                 .feature.isValid())
        {
            return 2;
        }
        const auto camera_type = fixture.awaitControl(fixture.control().registerFeatureType(kViewCameraFeatureFactory));
        if (!fixture
                 .awaitControl(
                     fixture.control().addFeature(scene.scene_id, camera_type.feature_type_id, ViewCameraCommTag{})
                 )
                 .feature.isValid())
        {
            return 3;
        }
        const auto camera_ops = ViewCameraOperationIds::fromOps(camera_type.ops, camera_type.op_count);
        if (!camera_ops.valid())
            return 4;

        const auto gbuffer_type =
            fixture.awaitControl(fixture.control().registerFeatureType(kDeferredGBufferFeatureFactory));
        DeferredGBufferCommConfig gbuffer_config{};
        if (!fixture
                 .awaitControl(
                     fixture.control().addFeature(scene.scene_id, gbuffer_type.feature_type_id, gbuffer_config)
                 )
                 .feature.isValid())
        {
            return 5;
        }
        const auto terrain_type = fixture.awaitControl(fixture.control().registerFeatureType(kTerrainFeatureFactory));
        const auto terrain_attached = fixture.awaitControl(
            fixture.control().addFeature(scene.scene_id, terrain_type.feature_type_id, TerrainCommConfig{})
        );
        if (!terrain_attached.feature.isValid())
        {
            std::fprintf(
                stderr,
                "Terrain feature attach failed: %s\n",
                formatRenderError(renderErrorRegistry(), terrain_attached.error).c_str()
            );
            return 6;
        }
        const auto terrain_ops = TerrainOperationIds::fromOps(terrain_type.ops, terrain_type.op_count);
        if (!terrain_ops.valid())
            return 7;

        const float view[16] =
            {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        const float projection[16] = {
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            -1.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            -1.001001f,
            -1.0f,
            0.0f,
            0.0f,
            -0.1001001f,
            0.0f};
        const float camera_position[3] = {0.0f, 50.0f, 0.0f};
        viewCameraUpdateTransient(
            ViewCameraProxy(fixture.session(), camera_ops),
            scene.scene_id,
            scene.view,
            view,
            projection,
            camera_position
        );

        UploadTerrainPagePayload upload{};
        upload.scene_id = scene.scene_id;
        upload.id.bytes[0] = 0x74u;
        upload.revision = 1u;
        upload.origin.local[0] = -64.0f;
        upload.origin.local[2] = -64.0f;
        upload.height_min = -10.0f;
        upload.height_max = 10.0f;
        upload.sample_spacing = 0.5f;
        upload.weight_layer_count = 1u;
        auto request = submitTerrain(fixture.uploadClientForTest(), terrain_ops, upload, makeTerrainPage());
        if (!request.valid())
            return 8;
        const auto uploaded = fixture.awaitUpload(std::move(request));
        if (uploaded.status != 0u || uploaded.cache_slot == 0xffffffffu)
            return 9;

        fixture.flush(10u);
        TerrainControlClient terrain_control{fixture.control(), terrain_ops};
        const auto stats = fixture.awaitControl(terrain_control.stats({scene.scene_id}));
        const bool is_invalid_resident_pages = stats.resident_pages != 1u;
        const bool is_invalid_full_resolution_pages = stats.full_resolution_pages != 1u;
        const bool is_invalid_patch_validity = stats.selected_patch_count_valid == 0u;
        const bool is_invalid_patch_count = stats.selected_patch_count != 64u;
        const bool is_empty_gpu_residency = stats.gpu_resident_bytes == 0u;
        const bool is_invalid_terrain_stats = is_invalid_resident_pages || is_invalid_full_resolution_pages ||
            is_invalid_patch_validity || is_invalid_patch_count || is_empty_gpu_residency;
        if (is_invalid_terrain_stats)
        {
            std::fprintf(
                stderr,
                "Terrain GPU stats mismatch: pages=%u full=%u valid=%u "
                "patches=%u gpu=%llu\n",
                stats.resident_pages,
                stats.full_resolution_pages,
                stats.selected_patch_count_valid,
                stats.selected_patch_count,
                static_cast<unsigned long long>(stats.gpu_resident_bytes)
            );
            return 10;
        }

        // Move the camera behind the page while preserving its orientation.
        // TerrainPatchSelect must reject every patch instead of relying on
        // fixed-function clipping after indirect expansion.
        const float identity_view[16] =
            {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        const float behind_position[3] = {0.0f, 50.0f, -10000.0f};
        viewCameraUpdateTransient(
            ViewCameraProxy(fixture.session(), camera_ops),
            scene.scene_id,
            scene.view,
            identity_view,
            projection,
            behind_position
        );
        fixture.flush(10u);
        const auto culled = fixture.awaitControl(terrain_control.stats({scene.scene_id}));
        if (culled.selected_patch_count_valid == 0u || culled.selected_patch_count != 0u)
        {
            std::fprintf(
                stderr,
                "Terrain frustum cull mismatch: valid=%u patches=%u\n",
                culled.selected_patch_count_valid,
                culled.selected_patch_count
            );
            return 11;
        }
        const auto removed = fixture.awaitControl(terrain_control.remove({scene.scene_id, upload.id, 2u}));
        if (removed.status != 0u)
            return 12;
        fixture.flush(4u);
        const auto empty = fixture.awaitControl(terrain_control.stats({scene.scene_id}));
        if (empty.resident_pages != 0u)
            return 13;
    }
    if (validation_errors.load(std::memory_order_acquire) != 0)
    {
        std::fprintf(
            stderr,
            "Terrain GPU probe saw %d Vulkan validation errors\n",
            validation_errors.load(std::memory_order_relaxed)
        );
        return 14;
    }
    return 0;
}
