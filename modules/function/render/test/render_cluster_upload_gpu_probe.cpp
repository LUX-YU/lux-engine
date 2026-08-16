#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdio>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    [[nodiscard]] lux::render::RenderRequest<
        lux::render::RenderClusterUploadedReply>
    submitCluster(
        const lux::render::RenderUploadClient& client,
        lux::render::RenderClusterOperationIds ops,
        lux::render::UploadRenderClusterPayload payload,
        std::shared_ptr<const std::vector<
            lux::render::RenderClusterWireInstance>> instances)
    {
        const auto bytes = instances->size() *
            sizeof(lux::render::RenderClusterWireInstance);
        auto submitted = client.trySubmit<
            lux::render::RenderClusterUploadedReply>(
            [instances, payload, operation_id = ops.id<
                 lux::render::RenderClusterUploadOp>()](
                lux::render::RenderUploadClient::Builder& builder) mutable
            {
                payload.instances = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(instances),
                    reinterpret_cast<const std::byte*>(instances->data()),
                    static_cast<std::uint32_t>(
                        instances->size() * sizeof(
                            lux::render::RenderClusterWireInstance)),
                    lux::render::attachment_types::OwnedBytes,
                    instances->size() * sizeof(
                        lux::render::RenderClusterWireInstance));
                builder.pushPreparedResource(operation_id, payload);
            },
            lux::render::UploadPayloadAccounting{
                .shared_bytes = bytes});
        if (!submitted)
        {
            std::fprintf(
                stderr,
                "Render Cluster upload admission failed (%u)\n",
                static_cast<unsigned>(submitted.error()));
            return {};
        }
        return std::move(*submitted);
    }

    [[nodiscard]] std::optional<double> lastPassMilliseconds(
        std::string_view json,
        std::string_view pass)
    {
        const std::string marker = "\"name\":\"" + std::string{pass} +
            "\",\"ms\":";
        const auto position = json.rfind(marker);
        if (position == std::string_view::npos)
            return std::nullopt;
        const auto* begin = json.data() + position + marker.size();
        const auto* end = json.data() + json.size();
        double value = 0.0;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{})
            return std::nullopt;
        return value;
    }

    [[nodiscard]] std::optional<double> latestCandidateMilliseconds(
        lux::rendertest::DeviceRenderFixture& fixture,
        lux::render::RenderSceneId scene)
    {
        std::string storage(2u * 1024u * 1024u, '\0');
        const auto reply = fixture.awaitControl(
            fixture.control().queryGpuTiming(
                scene,
                storage.data(),
                storage.size()));
        if (reply.status != 0u || reply.written == 0u ||
            reply.written >= storage.size())
        {
            return std::nullopt;
        }
        const std::string_view json{storage.data(), reply.written};
        const auto clear = lastPassMilliseconds(
            json, "RenderClusterCandidateClear");
        const auto append = lastPassMilliseconds(
            json, "RenderClusterCandidateAppend");
        const auto finalize = lastPassMilliseconds(
            json, "RenderClusterCandidateFinalize");
        if (!clear || !append || !finalize)
            return std::nullopt;
        return *clear + *append + *finalize;
    }

    [[nodiscard]] std::optional<std::string> gpuTimingJson(
        lux::rendertest::DeviceRenderFixture& fixture,
        lux::render::RenderSceneId scene)
    {
        std::string storage(2u * 1024u * 1024u, '\0');
        const auto reply = fixture.awaitControl(
            fixture.control().queryGpuTiming(
                scene,
                storage.data(),
                storage.size()));
        if (reply.status != 0u || reply.written == 0u ||
            reply.written >= storage.size())
        {
            return std::nullopt;
        }
        storage.resize(reply.written);
        return storage;
    }

    [[nodiscard]] std::optional<std::uint64_t> jsonUnsigned(
        std::string_view json,
        std::string_view name)
    {
        const std::string marker = "\"" + std::string{name} + "\":";
        const auto position = json.find(marker);
        if (position == std::string_view::npos)
            return std::nullopt;
        const auto* begin = json.data() + position + marker.size();
        const auto* end = json.data() + json.size();
        std::uint64_t value = 0u;
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{})
            return std::nullopt;
        return value;
    }

    [[nodiscard]] std::vector<std::uint32_t> loadSpirv(
        const char* path)
    {
        std::ifstream input{
            path,
            std::ios::binary | std::ios::ate};
        if (!input)
            return {};
        const auto bytes = static_cast<std::streamsize>(input.tellg());
        if (bytes <= 0 || bytes % sizeof(std::uint32_t) != 0)
            return {};
        std::vector<std::uint32_t> result(
            static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
        input.seekg(0, std::ios::beg);
        input.read(
            reinterpret_cast<char*>(result.data()),
            bytes);
        return input ? std::move(result) : std::vector<std::uint32_t>{};
    }

    [[nodiscard]] Eigen::Matrix4f buildViewMatrix(
        const Eigen::Vector3f& eye,
        const Eigen::Vector3f& target,
        const Eigen::Vector3f& up)
    {
        const Eigen::Vector3f forward = (target - eye).normalized();
        const Eigen::Vector3f side = forward.cross(up).normalized();
        const Eigen::Vector3f corrected_up = side.cross(forward);
        Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
        result(0, 0) = side.x();
        result(0, 1) = side.y();
        result(0, 2) = side.z();
        result(0, 3) = -side.dot(eye);
        result(1, 0) = corrected_up.x();
        result(1, 1) = corrected_up.y();
        result(1, 2) = corrected_up.z();
        result(1, 3) = -corrected_up.dot(eye);
        result(2, 0) = -forward.x();
        result(2, 1) = -forward.y();
        result(2, 2) = -forward.z();
        result(2, 3) = forward.dot(eye);
        return result;
    }
}

int main()
{
    using namespace lux::render;

    std::atomic<int> validation_errors{0};
    lux::rendertest::DeviceRenderFixture::Options fixture_options{};
    fixture_options.enable_validation = true;
    fixture_options.validation_errors = &validation_errors;
    fixture_options.capacity_request.set(
        lux::deployment::kActiveRenderInstancesCapacity,
        lux::deployment::RuntimeCapacityValue::exact(100'000u));
    fixture_options.capacity_request.set(
        lux::deployment::kClassicMeshRecordsCapacity,
        lux::deployment::RuntimeCapacityValue::exact(100'000u));
    lux::rendertest::DeviceRenderFixture fixture(
        128u,
        128u,
        "render_cluster_upload_gpu_probe",
        fixture_options);
    if (!fixture.ok())
    {
        std::printf("SKIP: Vulkan device unavailable\n");
        return 0;
    }

    const auto scene = fixture.makeSceneWithView(
        "RenderClusterProbe", "RenderClusterProbeView");
    const auto semantic_fragment = loadSpirv(
        LUX_RENDER_3D_SEMANTIC_FRAGMENT_SPV);
    if (semantic_fragment.empty())
        return 56;
    lux::rdesc::ShaderInfo semantic_info{};
    semantic_info.entry_points.push_back({
        "main",
        lux::rdesc::EShaderType::FRAGMENT});
    const auto semantic_info_bytes =
        lux::rdesc::ShaderInfo::serialize(semantic_info);
    const auto semantic_shader = fixture.awaitControl(
        fixture.control().compileShader(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(
                    semantic_fragment.data()),
                semantic_fragment.size() * sizeof(std::uint32_t)},
            semantic_info_bytes));
    if (semantic_shader.status != 0u || !semantic_shader.shader.isValid())
        return 57;
    const auto semantic_gbuffer_fragment = loadSpirv(
        LUX_RENDER_3D_SEMANTIC_GBUFFER_SPV);
    if (semantic_gbuffer_fragment.empty())
        return 59;
    const auto semantic_gbuffer_shader = fixture.awaitControl(
        fixture.control().compileShader(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(
                    semantic_gbuffer_fragment.data()),
                semantic_gbuffer_fragment.size() * sizeof(std::uint32_t)},
            semantic_info_bytes));
    if (semantic_gbuffer_shader.status != 0u ||
        !semantic_gbuffer_shader.shader.isValid())
    {
        return 60;
    }
    const auto camera_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kViewCameraFeatureFactory));
    const auto camera_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            camera_registered.feature_type_id,
            ViewCameraCommTag{}));
    if (!camera_attached.feature.isValid())
        return 1;
    const auto camera_ops = ViewCameraOperationIds::fromOps(
        camera_registered.ops, camera_registered.op_count);
    if (!camera_ops.valid())
        return 1;
    const auto material_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kMaterialFeatureFactory));
    const auto material_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            material_registered.feature_type_id,
            MaterialCommTag{}));
    if (!material_attached.feature.isValid())
        return 1;
    const auto material_ops = MaterialOperationIds::fromOps(
        material_registered.ops, material_registered.op_count);
    if (!material_ops.valid())
        return 1;

    std::vector<std::byte> texture_bytes(64u * 64u * 4u);
    std::fill(texture_bytes.begin(), texture_bytes.end(), std::byte{0x7f});
    auto texture_request = fixture.uploadClientForTest().tryCreateTexture2DCopy(
        texture_bytes,
        64,
        64,
        4,
        EPixelFormat::RGBA8_UNORM,
        true);
    if (!texture_request)
        return 33;
    const auto texture = fixture.awaitUpload(std::move(*texture_request));
    if (texture.status != 0u || texture.handle.isNull())
        return 34;

    GraphMaterialData graph_material{};
    graph_material.tex_bindless[0] = texture.handle.index;
    graph_material.tex_mask = 1u;
    auto material_request = uploadGraphMaterial(
        MaterialUploadClient(
            fixture.uploadClientForTest(), material_ops),
        graph_material,
        semantic_gbuffer_shader.shader,
        semantic_shader.shader);
    if (!material_request)
        return 35;
    const auto material = fixture.awaitUpload(std::move(*material_request));
    if (material.status != 0u || material.handle.isNull())
        return 36;
    const auto mesh_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kMeshStackFeatureFactory));
    const auto mesh_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            mesh_registered.feature_type_id,
            MeshStackCommTag{}));
    if (!mesh_attached.feature.isValid())
        return 2;
    const auto mesh_ops = MeshStackOperationIds::fromOps(
        mesh_registered.ops, mesh_registered.op_count);
    if (!mesh_ops.valid())
        return 3;

    // Closed, outward-wound sphere: sampling it from all six cardinal camera
    // directions simultaneously gates view convention and front-face state.
    lux::rdesc::Mesh triangle;
    constexpr std::uint32_t semantic_segments = 24u;
    constexpr std::uint32_t semantic_rings = 12u;
    constexpr float pi = 3.14159265358979323846f;
    for (std::uint32_t ring = 0u; ring <= semantic_rings; ++ring)
    {
        const float v = static_cast<float>(ring) / semantic_rings;
        const float phi = v * pi;
        const float sin_phi = std::sin(phi);
        const float cos_phi = std::cos(phi);
        for (std::uint32_t segment = 0u;
             segment <= semantic_segments;
             ++segment)
        {
            const float u = static_cast<float>(segment) /
                semantic_segments;
            const float theta = u * 2.0f * pi;
            const float sin_theta = std::sin(theta);
            const float cos_theta = std::cos(theta);
            auto& vertex = triangle.vertices.emplace_back();
            vertex.normal = {
                sin_phi * cos_theta,
                cos_phi,
                sin_phi * sin_theta};
            vertex.position = vertex.normal;
            vertex.tangent = {-sin_theta, 0.0f, cos_theta};
            vertex.bitangent = vertex.normal.cross(vertex.tangent).normalized();
            vertex.uv = {u, v};
            vertex.bone = {};
        }
    }
    const std::uint32_t semantic_stride = semantic_segments + 1u;
    for (std::uint32_t ring = 0u; ring < semantic_rings; ++ring)
    {
        for (std::uint32_t segment = 0u;
             segment < semantic_segments;
             ++segment)
        {
            const auto i0 = ring * semantic_stride + segment;
            const auto i1 = i0 + 1u;
            const auto i2 = i0 + semantic_stride;
            const auto i3 = i2 + 1u;
            triangle.indices.insert(
                triangle.indices.end(),
                {i0, i1, i2, i2, i1, i3});
        }
    }
    auto mesh_request = uploadMesh(
        MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
        triangle);
    if (!mesh_request)
        return 4;
    const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
    if (mesh.status != 0u || mesh.handle.isNull())
        return 5;

    // Fill the actual production-sized first VBO/IBO segments. The next mesh
    // must therefore allocate in segment 1; no test-only allocator size or
    // alternate resource path is involved.
    constexpr std::size_t initial_vbo_bytes = 64ull * 1024ull * 1024ull;
    constexpr std::size_t initial_ibo_bytes = 32ull * 1024ull * 1024ull;
    const auto sphere_vertex_bytes =
        triangle.vertices.size() * sizeof(lux::rdesc::Vertex);
    const auto sphere_index_bytes =
        triangle.indices.size() * sizeof(std::uint16_t);
    if (sphere_vertex_bytes >= initial_vbo_bytes ||
        sphere_index_bytes >= initial_ibo_bytes)
    {
        return 70;
    }
    lux::rdesc::Mesh segment_filler;
    segment_filler.vertices.resize(
        (initial_vbo_bytes - sphere_vertex_bytes) /
        sizeof(lux::rdesc::Vertex));
    const auto filler_index_count =
        ((initial_ibo_bytes - sphere_index_bytes) /
            sizeof(std::uint16_t) / 3u) * 3u;
    segment_filler.indices.assign(filler_index_count, 0u);
    mesh_request = uploadMesh(
        MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
        segment_filler);
    if (!mesh_request)
        return 71;
    const auto filler_mesh = fixture.awaitUpload(std::move(*mesh_request));
    if (filler_mesh.status != 0u || filler_mesh.handle.isNull())
        return 72;
    segment_filler = {};

    lux::rdesc::Mesh segmented_triangle;
    segmented_triangle.vertices.resize(65537u);
    segmented_triangle.vertices[0u].position = {-1.0f, -1.0f, 0.0f};
    segmented_triangle.vertices[1u].position = {1.0f, -1.0f, 0.0f};
    segmented_triangle.vertices[65536u].position = {0.0f, 1.0f, 0.0f};
    for (const auto index : {0u, 1u, 65536u})
    {
        auto& vertex = segmented_triangle.vertices[index];
        vertex.normal = {0.0f, 0.0f, 1.0f};
        vertex.tangent = {1.0f, 0.0f, 0.0f};
        vertex.bitangent = {0.0f, 1.0f, 0.0f};
    }
    segmented_triangle.indices = {0u, 1u, 65536u};
    mesh_request = uploadMesh(
        MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
        segmented_triangle);
    if (!mesh_request)
        return 73;
    const auto segmented_mesh = fixture.awaitUpload(
        std::move(*mesh_request));
    if (segmented_mesh.status != 0u || segmented_mesh.handle.isNull())
        return 74;

    MeshStackControlClient segment_stats_control{
        fixture.control(), mesh_ops};
    const auto segment_stats = fixture.awaitControl(
        segment_stats_control.stats({scene.scene_id}));
    if (segment_stats.vbo_segment_count < 2u ||
        segment_stats.ibo_segment_count < 2u ||
        segment_stats.vbo_growth_count == 0u ||
        segment_stats.ibo_growth_count == 0u)
    {
        std::fprintf(
            stderr,
            "Classic Mesh segment growth mismatch: vbo=%u ibo=%u vg=%u ig=%u\n",
            segment_stats.vbo_segment_count,
            segment_stats.ibo_segment_count,
            segment_stats.vbo_growth_count,
            segment_stats.ibo_growth_count);
        return 75;
    }

    const auto registered = fixture.awaitControl(
        fixture.control().registerFeatureType(
            kRenderClusterFeatureFactory));
    const auto attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            registered.feature_type_id,
            RenderClusterCommTag{}));
    if (!attached.feature.isValid())
    {
        std::fprintf(
            stderr,
            "Render Cluster feature attach failed: %s\n",
            formatRenderError(
                renderErrorRegistry(), attached.error).c_str());
        return 6;
    }
    const auto ops = RenderClusterOperationIds::fromOps(
        registered.ops, registered.op_count);
    if (!ops.valid())
        return 7;
    // Install the real consumer so the coarse cluster-cull -> candidate-expand
    // -> indirect instance-cull chain is retained and executed by RenderGraph.
    const auto forward_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kForwardMeshFeatureFactory));
    const auto light_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kLightFeatureFactory));
    if (!fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            light_registered.feature_type_id,
            LightCommTag{})).feature.isValid())
    {
        return 30;
    }
    const auto light_ops = LightOperationIds::fromOps(
        light_registered.ops, light_registered.op_count);
    if (!light_ops.valid())
        return 49;
    const auto shadow_map_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kShadowMapFeatureFactory));
    ShadowMapCommConfig shadow_map_config{};
    shadow_map_config.atlas_page_resolution = 256u;
    shadow_map_config.atlas_page_count = 1u;
    const auto shadow_map_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            shadow_map_registered.feature_type_id,
            shadow_map_config));
    if (!shadow_map_attached.feature.isValid())
        return 30;
    const auto mesh_shadow_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kMeshShadowFeatureFactory));
    MeshShadowCommConfig mesh_shadow_config{};
    const auto mesh_shadow_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            mesh_shadow_registered.feature_type_id,
            mesh_shadow_config));
    if (!mesh_shadow_attached.feature.isValid())
        return 30;
    ForwardMeshCommConfig forward_config{};
    forward_config.graph_fragment = semantic_shader.shader;
    const auto forward_attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            forward_registered.feature_type_id,
            forward_config));
    if (!forward_attached.feature.isValid())
    {
        std::fprintf(
            stderr,
            "Forward feature attach failed: %s\n",
            formatRenderError(
                renderErrorRegistry(), forward_attached.error).c_str());
        return 31;
    }

    auto instances = std::make_shared<std::vector<
        RenderClusterWireInstance>>(2u);
    (*instances)[0].mesh = mesh.handle;
    (*instances)[1].mesh = segmented_mesh.handle;
    (*instances)[0].material = material.handle;
    (*instances)[1].material = material.handle;
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float instance_transform[16]{};
    std::copy(std::begin(identity), std::end(identity), instance_transform);
    instance_transform[14] = -5.0f;
    (*instances)[0].transform =
        makeTransientRenderSpatialTransform3D(instance_transform);
    (*instances)[1].transform =
        makeTransientRenderSpatialTransform3D(instance_transform);
    (*instances)[0].stable_pick_id = 10u;
    (*instances)[1].stable_pick_id = 11u;
    (*instances)[0].rgba8 = 0xff0000ffu;
    (*instances)[1].rgba8 = 0xff0000ffu;
    UploadRenderClusterPayload upload{};
    upload.scene_id = scene.scene_id;
    upload.id.bytes[0] = 0x51u;
    upload.revision = 1u;
    upload.bounds_radius = 10.0f;
    // This probe validates cluster upload/cull/picking. Keep its representation
    // immediately visible; transition coverage has its own assertions below.
    upload.transition_milliseconds = 0u;
    upload.instance_count = 2u;
    auto upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, upload, instances);
    if (!upload_request.valid())
        return 8;
    const auto uploaded = fixture.awaitUpload(std::move(upload_request));
    if (uploaded.status != 0u || uploaded.instance_count != 2u)
        return 9;

    float dynamic_transform[16]{};
    std::copy(std::begin(identity), std::end(identity), dynamic_transform);
    dynamic_transform[12] = 20.0f;
    dynamic_transform[14] = -5.0f;
    const auto dynamic_instance = fixture.await(addTransientMeshInstance(
        MeshStackProxy(fixture.session(), mesh_ops),
        scene.scene_id,
        mesh.handle,
        material.handle,
        dynamic_transform,
        kInstanceFlagCastShadow |
            kInstanceFlagReceiveShadow |
            kInstanceFlagVisible |
            (1u << 31u)));
    if (dynamic_instance.status != MeshInstanceCreateStatus::Ok ||
        !dynamic_instance.object)
    {
        return 40;
    }

    RenderClusterControlClient control{fixture.control(), ops};
    auto stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 1u || stats.instance_count != 2u ||
        stats.visible_cluster_count != 1u ||
        stats.visible_instance_count != 2u)
        return 10;

    const float camera_position[3] = {0.0f, 0.0f, 0.0f};
    const float projection[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.001001f, -1.0f,
        0.0f, 0.0f, -0.1001001f, 0.0f};
    viewCameraUpdateTransient(
        ViewCameraProxy(fixture.session(), camera_ops),
        scene.scene_id,
        scene.view,
        identity,
        projection,
        camera_position);
    DirectionalLightDesc semantic_light{};
    semantic_light.direction = {0.0f, 0.0f, -1.0f};
    semantic_light.intensity = 4.0f;
    semantic_light.flags = 0u;
    const auto semantic_light_reply = fixture.await(lightCreate(
        LightProxy(fixture.session(), light_ops),
        scene.scene_id,
        LightDescriptor{semantic_light}));
    if (semantic_light_reply.status != 0u ||
        semantic_light_reply.handle.isNull())
    {
        return 55;
    }
    constexpr std::uint64_t pick_generation = 41u;
    RenderClusterProxy(fixture.session(), ops).requestPick(
        RequestRenderClusterPickPayload{
            .scene_id = scene.scene_id,
            .view_index = scene.view.index,
            .view_generation = scene.view.gen,
            .request_generation = pick_generation,
            .normalized_x = 0.5f,
            .normalized_y = 0.5f,
            .maximum_distance = 100.0f});
    fixture.flush(8);
    struct CameraCase final
    {
        const char* name;
        Eigen::Vector3f direction;
        Eigen::Vector3f up;
    };
    const std::array camera_cases{
        CameraCase{"+X", { 1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        CameraCase{"-X", {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        CameraCase{"+Y", { 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f,-1.0f}},
        CameraCase{"-Y", { 0.0f,-1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        CameraCase{"+Z", { 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        CameraCase{"-Z", { 0.0f, 0.0f,-1.0f}, {0.0f, 1.0f, 0.0f}}};
    const Eigen::Vector3f semantic_center{0.0f, 0.0f, -5.0f};
    for (const auto& camera_case : camera_cases)
    {
        const Eigen::Vector3f eye = semantic_center +
            camera_case.direction * 4.0f;
        const auto view_matrix = buildViewMatrix(
            eye,
            semantic_center,
            camera_case.up);
        for (std::uint32_t frame = 0u; frame < 4u; ++frame)
        {
            viewCameraUpdateTransient(
                ViewCameraProxy(fixture.session(), camera_ops),
                scene.scene_id,
                scene.view,
                view_matrix.data(),
                projection,
                eye.data());
            fixture.flush();
        }
        const auto semantic_pixels = fixture.readback(scene);
        std::size_t semantic_bright = 0u;
        std::size_t semantic_red = 0u;
        for (std::uint32_t y = 24u; y < 104u; ++y)
        {
            for (std::uint32_t x = 24u; x < 104u; ++x)
            {
                const auto index =
                    (static_cast<std::size_t>(y) * 128u + x) * 4u;
                const int blue = semantic_pixels[index];
                const int green = semantic_pixels[index + 1u];
                const int red = semantic_pixels[index + 2u];
                if (std::max({blue, green, red}) > 32)
                    ++semantic_bright;
                if (red > 48 && red > green + 24 && red > blue + 24)
                    ++semantic_red;
            }
        }
        std::printf(
            "Forward semantic %s: bright=%zu red=%zu status=%u\n",
            camera_case.name,
            semantic_bright,
            semantic_red,
            fixture.lastReadback().status);
        if (fixture.lastReadback().status != 0u ||
            semantic_bright < 250u || semantic_red < 250u)
        {
            return 58;
        }
    }
    const auto countSemanticRedPixels = [&]() -> std::size_t
    {
        fixture.flush(4);
        const auto pixels = fixture.readback(scene);
        if (fixture.lastReadback().status != 0u)
            return 0u;
        std::size_t red_pixels = 0u;
        for (std::uint32_t y = 24u; y < 104u; ++y)
        {
            for (std::uint32_t x = 24u; x < 104u; ++x)
            {
                const auto index =
                    (static_cast<std::size_t>(y) * 128u + x) * 4u;
                const int blue = pixels[index];
                const int green = pixels[index + 1u];
                const int red = pixels[index + 2u];
                if (red > 48 && red > green + 24 && red > blue + 24)
                    ++red_pixels;
            }
        }
        return red_pixels;
    };

    // A second offscreen scene exercises the GBuffer -> DeferredLighting path
    // with the same mesh, material, tint and canonical camera convention. The
    // GBuffer shader emits the renderer client's Unlit model id, so the final
    // region colour is deterministic and directly comparable to Forward.
    RenderControlSession::CreateSceneConfig deferred_scene_config{};
    deferred_scene_config.name = "Render3DSemanticDeferred";
    deferred_scene_config.lit_color_format =
        lux::common::ETextureFormat::RGBA8_SRGB;
    const auto deferred_scene = fixture.makeSceneWithView(
        deferred_scene_config,
        "Render3DSemanticDeferredView");
    const auto addExistingFeature = [&](auto registration, const auto& config)
    {
        return fixture.awaitControl(fixture.control().addFeature(
            deferred_scene.scene_id,
            registration.feature_type_id,
            config));
    };
    if (!addExistingFeature(light_registered, LightCommTag{}).feature.isValid() ||
        !addExistingFeature(camera_registered, ViewCameraCommTag{}).feature.isValid() ||
        !addExistingFeature(material_registered, MaterialCommTag{}).feature.isValid() ||
        !addExistingFeature(mesh_registered, MeshStackCommTag{}).feature.isValid() ||
        !addExistingFeature(registered, RenderClusterCommTag{}).feature.isValid())
    {
        return 61;
    }
    ShadowMapCommConfig deferred_shadow_config{};
    deferred_shadow_config.atlas_page_resolution = 256u;
    deferred_shadow_config.atlas_page_count = 1u;
    if (!addExistingFeature(
            shadow_map_registered,
            deferred_shadow_config).feature.isValid() ||
        !addExistingFeature(
            mesh_shadow_registered,
            MeshShadowCommConfig{}).feature.isValid())
    {
        return 62;
    }
    const auto gbuffer_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(
            kDeferredGBufferFeatureFactory));
    DeferredGBufferCommConfig gbuffer_config{};
    gbuffer_config.gbuffer_graph_fragment_shader =
        semantic_gbuffer_shader.shader;
    if (!addExistingFeature(
            gbuffer_registered,
            gbuffer_config).feature.isValid())
    {
        return 63;
    }
    const auto deferred_registered = fixture.awaitControl(
        fixture.control().registerFeatureType(
            kDeferredLightingFeatureFactory));
    DeferredLightingCommConfig deferred_config{};
    deferred_config.read_mode = ELightingReadMode::SAMPLED;
    deferred_config.enable_clustered = 0u;
    if (!addExistingFeature(
            deferred_registered,
            deferred_config).feature.isValid())
    {
        return 64;
    }

    auto deferred_instances = std::make_shared<std::vector<
        RenderClusterWireInstance>>(1u, (*instances)[1]);
    UploadRenderClusterPayload deferred_upload{};
    deferred_upload.scene_id = deferred_scene.scene_id;
    deferred_upload.id.bytes[0] = 0x52u;
    deferred_upload.revision = 1u;
    deferred_upload.bounds_center.local[2] = -5.0f;
    deferred_upload.bounds_radius = 2.0f;
    deferred_upload.transition_milliseconds = 0u;
    deferred_upload.instance_count = 1u;
    auto deferred_upload_request = submitCluster(
        fixture.uploadClientForTest(),
        ops,
        deferred_upload,
        deferred_instances);
    if (!deferred_upload_request.valid())
        return 65;
    const auto deferred_uploaded = fixture.awaitUpload(
        std::move(deferred_upload_request));
    if (deferred_uploaded.status != 0u ||
        deferred_uploaded.instance_count != 1u)
    {
        return 66;
    }
    const Eigen::Vector3f deferred_eye{0.0f, 0.0f, -1.0f};
    const auto deferred_view = buildViewMatrix(
        deferred_eye,
        semantic_center,
        Eigen::Vector3f{0.0f, 1.0f, 0.0f});
    for (std::uint32_t frame = 0u; frame < 8u; ++frame)
    {
        viewCameraUpdateTransient(
            ViewCameraProxy(fixture.session(), camera_ops),
            deferred_scene.scene_id,
            deferred_scene.view,
            deferred_view.data(),
            projection,
            deferred_eye.data());
        fixture.flush();
    }
    const auto deferred_pixels = fixture.readback(deferred_scene);
    std::size_t deferred_red = 0u;
    std::size_t deferred_bright = 0u;
    for (std::uint32_t y = 24u; y < 104u; ++y)
    {
        for (std::uint32_t x = 24u; x < 104u; ++x)
        {
            const auto index =
                (static_cast<std::size_t>(y) * 128u + x) * 4u;
            const int blue = deferred_pixels[index];
            const int green = deferred_pixels[index + 1u];
            const int red = deferred_pixels[index + 2u];
            if (std::max({blue, green, red}) > 32)
                ++deferred_bright;
            if (red > 48 && red > green + 24 && red > blue + 24)
                ++deferred_red;
        }
    }
    const auto deferred_stats = fixture.awaitControl(
        RenderClusterControlClient{fixture.control(), ops}.stats({
            deferred_scene.scene_id}));
    std::string deferred_timing_storage(512u * 1024u, '\0');
    const auto deferred_timing_reply = fixture.awaitControl(
        fixture.control().queryGpuTiming(
            deferred_scene.scene_id,
            deferred_timing_storage.data(),
            deferred_timing_storage.size()));
    const std::string_view deferred_timing{
        deferred_timing_storage.data(),
        deferred_timing_reply.written};
    const auto gbuffer_ms = lastPassMilliseconds(
        deferred_timing,
        "DeferredGBufferDraw");
    const auto deferred_ms = lastPassMilliseconds(
        deferred_timing,
        "DeferredLighting");
    std::string deferred_graph_storage(512u * 1024u, '\0');
    const auto deferred_graph_reply = fixture.awaitControl(
        fixture.control().dumpRenderGraph(
            deferred_scene.scene_id,
            deferred_graph_storage.data(),
            deferred_graph_storage.size()));
    const std::string_view deferred_graph{
        deferred_graph_storage.data(),
        deferred_graph_reply.written};
    const bool graph_has_gbuffer =
        deferred_graph.find("DeferredGBufferDraw") != std::string_view::npos;
    const bool graph_has_lighting =
        deferred_graph.find("DeferredLighting") != std::string_view::npos;
    std::printf(
        "GBuffer semantic: bright=%zu red=%zu status=%u clusters=%u instances=%u "
        "candidate=%u/%u cull=%u/%u/%u timing=%.6f/%.6f graph=%u/%u\n",
        deferred_bright,
        deferred_red,
        fixture.lastReadback().status,
        deferred_stats.cluster_count,
        deferred_stats.instance_count,
        deferred_stats.gpu_candidate_count,
        deferred_stats.gpu_candidate_count_valid,
        deferred_stats.cull_visible_flag_count,
        deferred_stats.cull_mdc_count,
        deferred_stats.cull_frustum_count,
        gbuffer_ms.value_or(-1.0),
        deferred_ms.value_or(-1.0),
        graph_has_gbuffer ? 1u : 0u,
        graph_has_lighting ? 1u : 0u);
    if (fixture.lastReadback().status != 0u || deferred_red < 250u ||
        deferred_stats.cluster_count != 1u ||
        deferred_stats.instance_count != 1u)
    {
        return 67;
    }
    const auto gpu_stats = fixture.awaitControl(
        control.stats({scene.scene_id}));
    if (gpu_stats.gpu_candidate_count_valid == 0u ||
        gpu_stats.gpu_candidate_count != 3u ||
        gpu_stats.gpu_candidate_requested_count != 3u ||
        gpu_stats.gpu_candidate_overflow_count != 0u)
    {
        std::fprintf(
            stderr,
            "Cluster candidate count mismatch: valid=%u count=%u\n",
            gpu_stats.gpu_candidate_count_valid,
            gpu_stats.gpu_candidate_count);
        return 32;
    }

    MeshStackProxy(fixture.session(), mesh_ops).removeMeshInstance(
        RemoveMeshInstancePayload{
            .scene_id = scene.scene_id,
            .object = dynamic_instance.object});
    fixture.flush(8);
    const auto post_remove_stats = fixture.awaitControl(
        control.stats({scene.scene_id}));
    if (post_remove_stats.gpu_candidate_count_valid == 0u ||
        post_remove_stats.gpu_candidate_count != 2u ||
        post_remove_stats.gpu_candidate_requested_count != 2u ||
        post_remove_stats.gpu_candidate_overflow_count != 0u)
    {
        std::fprintf(
            stderr,
            "Dynamic candidate removal mismatch: valid=%u count=%u\n",
            post_remove_stats.gpu_candidate_count_valid,
            post_remove_stats.gpu_candidate_count);
        return 41;
    }

    // A persistent World actor does not remain in EnTT after unload, but its
    // render slot becomes a bounded ghost. Verify that the slot and its render
    // resource bindings survive the retirement edge, then that expiry reclaims
    // both without touching the two cluster-owned instances.
    const auto createFadeProbe = [&](std::uint32_t seed)
    {
        return fixture.await(addMeshInstance(
            MeshStackProxy(fixture.session(), mesh_ops),
            scene.scene_id,
            mesh.handle,
            material.handle,
            makeTransientRenderSpatialTransform3D(dynamic_transform),
            kInstanceFlagCastShadow |
                kInstanceFlagReceiveShadow |
                kInstanceFlagVisible,
            EGeometryKind::StaticMesh,
            kPassMaskOpaqueDefault,
            ~0u,
            1u,
            seed));
    };
    MeshStackControlClient mesh_stats_control{
        fixture.control(), mesh_ops};
    const auto held_fade = createFadeProbe(0x1234u);
    if (held_fade.status != MeshInstanceCreateStatus::Ok ||
        !held_fade.object)
    {
        return 42;
    }
    MeshStackProxy(fixture.session(), mesh_ops).retireMeshInstance({
        .scene_id = scene.scene_id,
        .object = held_fade.object,
        .transition_milliseconds = 10000u,
        .transition_seed = 0x1234u});
    fixture.flush();
    const auto held_stats = fixture.awaitControl(
        mesh_stats_control.stats({scene.scene_id}));
    if (held_stats.alive_instances != 3u ||
        held_stats.cluster_owned_instances != 2u ||
        held_stats.transitioning_instances != 1u ||
        held_stats.resource_bound_instances != 3u)
    {
        std::fprintf(
            stderr,
            "Fade hold mismatch: alive=%u cluster=%u transitions=%u binds=%u\n",
            held_stats.alive_instances,
            held_stats.cluster_owned_instances,
            held_stats.transitioning_instances,
            held_stats.resource_bound_instances);
        return 43;
    }
    MeshStackProxy(fixture.session(), mesh_ops).removeMeshInstance({
        .scene_id = scene.scene_id,
        .object = held_fade.object});
    fixture.flush();

    const auto expiring_fade = createFadeProbe(0x5678u);
    if (expiring_fade.status != MeshInstanceCreateStatus::Ok ||
        !expiring_fade.object)
    {
        return 44;
    }
    MeshStackProxy(fixture.session(), mesh_ops).retireMeshInstance({
        .scene_id = scene.scene_id,
        .object = expiring_fade.object,
        .transition_milliseconds = 50u,
        .transition_seed = 0x5678u});
    fixture.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    fixture.flush();
    const auto expired_stats = fixture.awaitControl(
        mesh_stats_control.stats({scene.scene_id}));
    if (expired_stats.alive_instances != 2u ||
        expired_stats.cluster_owned_instances != 2u ||
        expired_stats.transitioning_instances != 0u ||
        expired_stats.resource_bound_instances != 2u)
    {
        std::fprintf(
            stderr,
            "Fade expiry mismatch: alive=%u cluster=%u transitions=%u binds=%u\n",
            expired_stats.alive_instances,
            expired_stats.cluster_owned_instances,
            expired_stats.transitioning_instances,
            expired_stats.resource_bound_instances);
        return 45;
    }

    // Local World lights follow the same render-owner lifetime rule as mesh
    // ghosts: a retirement edge keeps the SSBO slot alive while intensity
    // falls to zero, but no ECS entity or client-side polling is required.
    PointLightDesc fading_light{};
    fading_light.spatial_position.local[2] = -4.0f;
    fading_light.intensity = 8.0f;
    fading_light.range = 12.0f;
    auto light_reply = fixture.await(lightCreate(
        LightProxy(fixture.session(), light_ops),
        scene.scene_id,
        LightDescriptor{fading_light},
        10000u));
    if (light_reply.status != 0u || light_reply.handle.isNull())
        return 50;
    LightControlClient light_control{fixture.control(), light_ops};
    LightProxy(fixture.session(), light_ops).destroyLight({
        .scene_id = scene.scene_id,
        .handle = light_reply.handle,
        .transition_milliseconds = 10000u});
    fixture.flush();
    auto light_stats = fixture.awaitControl(
        light_control.stats({scene.scene_id}));
    if (light_stats.point_lights != 1u ||
        light_stats.transitioning_lights != 1u)
    {
        std::fprintf(
            stderr,
            "Light fade hold mismatch: point=%u transitions=%u\n",
            light_stats.point_lights,
            light_stats.transitioning_lights);
        return 51;
    }
    // An explicit zero-duration destroy is also the teardown/cancellation
    // path and must reclaim a previously transitioning slot immediately.
    LightProxy(fixture.session(), light_ops).destroyLight({
        .scene_id = scene.scene_id,
        .handle = light_reply.handle,
        .transition_milliseconds = 0u});
    fixture.flush();
    light_stats = fixture.awaitControl(
        light_control.stats({scene.scene_id}));
    if (light_stats.point_lights != 0u ||
        light_stats.transitioning_lights != 0u)
    {
        return 52;
    }

    light_reply = fixture.await(lightCreate(
        LightProxy(fixture.session(), light_ops),
        scene.scene_id,
        LightDescriptor{fading_light}));
    if (light_reply.status != 0u || light_reply.handle.isNull())
        return 53;
    LightProxy(fixture.session(), light_ops).destroyLight({
        .scene_id = scene.scene_id,
        .handle = light_reply.handle,
        .transition_milliseconds = 50u});
    fixture.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    fixture.flush();
    light_stats = fixture.awaitControl(
        light_control.stats({scene.scene_id}));
    if (light_stats.point_lights != 0u ||
        light_stats.transitioning_lights != 0u)
    {
        std::fprintf(
            stderr,
            "Light fade expiry mismatch: point=%u transitions=%u\n",
            light_stats.point_lights,
            light_stats.transitioning_lights);
        return 54;
    }
    if (gpu_stats.wanted_mip_feedback_valid == 0u ||
        gpu_stats.wanted_mip_texture_count != 1u ||
        gpu_stats.minimum_wanted_mip != 0u ||
        gpu_stats.workgroup_aggregation_fallback_count != 0u)
    {
        std::fprintf(
            stderr,
            "Wanted-mip feedback mismatch: valid=%u count=%u mip=%u fallback=%u\n",
            gpu_stats.wanted_mip_feedback_valid,
            gpu_stats.wanted_mip_texture_count,
            gpu_stats.minimum_wanted_mip,
            gpu_stats.workgroup_aggregation_fallback_count);
        return 37;
    }

    // Replace the physical allocation at logical mip 2. The bindless handle
    // must remain stable while actual bytes fall below the full logical chain.
    std::vector<std::byte> coarse_texture_bytes(16u * 16u * 4u);
    std::fill(
        coarse_texture_bytes.begin(),
        coarse_texture_bytes.end(),
        std::byte{0x3f});
    std::vector<OwnedTextureMipLevel> coarse_mips;
    coarse_mips.push_back(OwnedTextureMipLevel{
        lux::cxx::SharedBytes<>::copyOf(coarse_texture_bytes),
        16u,
        16u});
    auto replace_request = fixture.uploadClientForTest()
        .tryReplaceTexture2DMipRange(
            texture.handle,
            2u,
            std::move(coarse_mips),
            EPixelFormat::RGBA8_UNORM,
            true);
    if (!replace_request)
        return 38;
    const auto coarse_replaced = fixture.awaitUpload(
        std::move(*replace_request));
    if (coarse_replaced.status != 0u ||
        coarse_replaced.handle.index != texture.handle.index ||
        coarse_replaced.handle.gen != texture.handle.gen ||
        coarse_replaced.base_mip != 2u)
    {
        return 39;
    }
    fixture.flush(8);
    const auto coarse_stats = fixture.awaitControl(
        control.stats({scene.scene_id}));
    if (coarse_stats.actual_texture_bytes >=
            coarse_stats.full_texture_bytes ||
        coarse_stats.actual_texture_bytes == 0u)
    {
        std::fprintf(
            stderr,
            "Mip replacement did not reduce bytes: actual=%llu full=%llu\n",
            static_cast<unsigned long long>(
                coarse_stats.actual_texture_bytes),
            static_cast<unsigned long long>(
                coarse_stats.full_texture_bytes));
        return 40;
    }
    const auto mip_demands = fixture.awaitControl(
        fixture.control().request<TextureMipDemandsReply>(
            opcodes::ResourceOp,
            type_ids::QueryTextureMipDemands,
            QueryTextureMipDemandsPayload{}));
    const auto matching_demand = std::find_if(
        mip_demands.entries.begin(),
        mip_demands.entries.begin() + mip_demands.count,
        [&](const TextureMipDemandEntry& entry)
        {
            return entry.handle.index == texture.handle.index &&
                entry.handle.gen == texture.handle.gen;
        });
    if (matching_demand ==
            mip_demands.entries.begin() + mip_demands.count ||
        matching_demand->resident_base_mip != 2u ||
        matching_demand->target_base_mip != 0u)
    {
        return 47;
    }

    // A shape-invalid replacement is rejected before transfer admission and
    // must leave the coarse image live (same handle, same actual bytes).
    std::vector<OwnedTextureMipLevel> invalid_mips;
    invalid_mips.push_back(OwnedTextureMipLevel{
        lux::cxx::SharedBytes<>::copyOf(coarse_texture_bytes),
        16u,
        16u});
    replace_request = fixture.uploadClientForTest()
        .tryReplaceTexture2DMipRange(
            texture.handle,
            3u,
            std::move(invalid_mips),
            EPixelFormat::RGBA8_UNORM,
            true);
    if (!replace_request)
        return 41;
    const auto invalid_replaced = fixture.awaitUpload(
        std::move(*replace_request));
    if (invalid_replaced.status == 0u)
        return 42;
    fixture.flush(4);
    const auto after_invalid = fixture.awaitControl(
        control.stats({scene.scene_id}));
    if (after_invalid.actual_texture_bytes !=
        coarse_stats.actual_texture_bytes)
    {
        return 43;
    }

    std::vector<OwnedTextureMipLevel> full_mips;
    full_mips.push_back(OwnedTextureMipLevel{
        lux::cxx::SharedBytes<>::copyOf(texture_bytes),
        64u,
        64u});
    replace_request = fixture.uploadClientForTest()
        .tryReplaceTexture2DMipRange(
            texture.handle,
            0u,
            std::move(full_mips),
            EPixelFormat::RGBA8_UNORM,
            true);
    if (!replace_request)
        return 44;
    const auto full_replaced = fixture.awaitUpload(
        std::move(*replace_request));
    if (full_replaced.status != 0u ||
        full_replaced.handle.index != texture.handle.index ||
        full_replaced.handle.gen != texture.handle.gen ||
        full_replaced.base_mip != 0u)
    {
        return 45;
    }
    fixture.flush(8);
    const auto full_stats = fixture.awaitControl(
        control.stats({scene.scene_id}));
    if (full_stats.actual_texture_bytes !=
        full_stats.full_texture_bytes)
    {
        std::fprintf(
            stderr,
            "Mip restoration mismatch: actual=%llu full=%llu\n",
            static_cast<unsigned long long>(
                full_stats.actual_texture_bytes),
            static_cast<unsigned long long>(
                full_stats.full_texture_bytes));
        return 46;
    }
    const auto cleared_demands = fixture.awaitControl(
        fixture.control().request<TextureMipDemandsReply>(
            opcodes::ResourceOp,
            type_ids::QueryTextureMipDemands,
            QueryTextureMipDemandsPayload{}));
    if (std::any_of(
            cleared_demands.entries.begin(),
            cleared_demands.entries.begin() + cleared_demands.count,
            [&](const TextureMipDemandEntry& entry)
            {
                return entry.handle.index == texture.handle.index &&
                    entry.handle.gen == texture.handle.gen;
            }))
    {
        return 48;
    }
    const auto picked = fixture.awaitControl(control.pickResult({
        scene.scene_id, pick_generation}));
    if (picked.status != ERenderPickStatus::HIT ||
        picked.stable_pick_id != 10u ||
        picked.view_generation != scene.view.gen ||
        !(picked.depth > 0.0f && picked.depth < 50.0f))
    {
        std::fprintf(
            stderr,
            "Render Cluster pick mismatch: status=%u id=%llu view=%u depth=%f\n",
            static_cast<unsigned>(picked.status),
            static_cast<unsigned long long>(picked.stable_pick_id),
            picked.view_generation,
            picked.depth);
        return 28;
    }

    constexpr std::uint64_t stale_pick_generation = 42u;
    RenderClusterProxy(fixture.session(), ops).requestPick(
        RequestRenderClusterPickPayload{
            .scene_id = scene.scene_id,
            .view_index = scene.view.index,
            .view_generation = scene.view.gen + 1u,
            .request_generation = stale_pick_generation,
            .normalized_x = 0.5f,
            .normalized_y = 0.5f,
            .maximum_distance = 100.0f});
    fixture.flush();
    const auto stale_pick = fixture.awaitControl(control.pickResult({
        scene.scene_id, stale_pick_generation}));
    if (stale_pick.status != ERenderPickStatus::STALE)
        return 29;

    const auto removed = fixture.awaitControl(
        control.remove({scene.scene_id, upload.id, 3u}));
    if (removed.status != 0u || removed.revision != 3u)
        return 11;
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 0u || stats.instance_count != 0u)
        return 12;

    upload.revision = 2u;
    upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, upload, instances);
    if (!upload_request.valid())
        return 13;
    const auto stale = fixture.awaitUpload(std::move(upload_request));
    if (stale.status != 0u)
        return 14;
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 0u || stats.instance_count != 0u)
        return 15;

    upload.revision = 4u;
    upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, upload, instances);
    if (!upload_request.valid())
        return 16;
    const auto fresh = fixture.awaitUpload(std::move(upload_request));
    if (fresh.status != 0u)
        return 17;
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 1u || stats.instance_count != 2u)
        return 18;
    const auto final_removed = fixture.awaitControl(
        control.remove({scene.scene_id, upload.id, 5u}));
    if (final_removed.status != 0u)
        return 19;
    fixture.flush(2);

    // Candidate scan probes: exact counts at 4k/10k/100k, no capacity
    // overflow, and a VkDispatchIndirectCommand consistent with accepted.
    // Removing between sizes keeps the capacity contract in active-object
    // terms (replacement transactions may temporarily hold old+new revisions)
    // and exercises immediate slot reuse without graph capacity growth.
    UploadRenderClusterPayload stress = upload;
    std::uint8_t probe_id = 0x70u;
    constexpr std::array candidate_probe_counts{
        4'000u, 10'000u, 100'000u};
    std::array<double, candidate_probe_counts.size()>
        candidate_probe_milliseconds{};
    std::size_t candidate_probe_index = 0u;
    for (const std::uint32_t count : candidate_probe_counts)
    {
        stress.id = {};
        stress.id.bytes[0] = probe_id++;
        stress.revision = 1u;
        auto stress_instances = std::make_shared<std::vector<
            RenderClusterWireInstance>>(count, (*instances)[0]);
        stress.instance_count = count;
        upload_request = submitCluster(
            fixture.uploadClientForTest(),
            ops,
            stress,
            stress_instances);
        if (!upload_request.valid())
        {
            std::fprintf(stderr, "Candidate probe %u upload request invalid\n", count);
            return 55;
        }
        const auto stress_uploaded = fixture.awaitUpload(
            std::move(upload_request));
        if (stress_uploaded.status != 0u)
        {
            std::fprintf(
                stderr,
                "Candidate probe %u upload rejected: status=%u\n",
                count,
                stress_uploaded.status);
            return 55;
        }
        fixture.flush(8);
        const auto candidate_stats = fixture.awaitControl(
            control.stats({scene.scene_id}));
        const auto expected_groups = (count + 63u) / 64u;
        if (candidate_stats.gpu_candidate_count_valid == 0u ||
            candidate_stats.gpu_candidate_requested_count != count ||
            candidate_stats.gpu_candidate_count != count ||
            candidate_stats.gpu_candidate_overflow_count != 0u ||
            candidate_stats.gpu_candidate_group_count != expected_groups)
        {
            std::fprintf(
                stderr,
                "Candidate probe %u mismatch: requested=%u accepted=%u "
                "overflow=%u groups=%u valid=%u\n",
                count,
                candidate_stats.gpu_candidate_requested_count,
                candidate_stats.gpu_candidate_count,
                candidate_stats.gpu_candidate_overflow_count,
                candidate_stats.gpu_candidate_group_count,
                candidate_stats.gpu_candidate_count_valid);
            return 56;
        }
        const auto timing = latestCandidateMilliseconds(
            fixture,
            scene.scene_id);
        if (!timing)
        {
            std::fprintf(
                stderr,
                "Candidate probe %u has no correlated GPU timing\n",
                count);
            return 58;
        }
        candidate_probe_milliseconds[candidate_probe_index++] = *timing;
        std::uint64_t remove_revision = stress.revision + 1u;
        if (count == 100'000u)
        {
            const auto graph_before_revision_json = gpuTimingJson(
                fixture,
                scene.scene_id);
            const auto graph_compiles_before_revision =
                graph_before_revision_json
                ? jsonUnsigned(
                      *graph_before_revision_json,
                      "compile_successes")
                : std::nullopt;
            if (!graph_compiles_before_revision)
                return 72;
            // Replace the complete active revision before releasing it. The
            // old revision remains drawable until the 100k replacement has
            // passed admission and committed at the render safe point.
            stress.revision = 2u;
            (*stress_instances)[0u].rgba8 ^= 0x00010101u;
            auto replacement_request = submitCluster(
                fixture.uploadClientForTest(),
                ops,
                stress,
                stress_instances);
            if (!replacement_request.valid() ||
                fixture.awaitUpload(std::move(replacement_request)).status != 0u)
            {
                std::fprintf(stderr, "100k revision replacement failed\n");
                return 73;
            }
            const auto replacement_stats = fixture.awaitControl(
                control.stats({scene.scene_id}));
            if (replacement_stats.cluster_count != 1u ||
                replacement_stats.instance_count != count)
            {
                return 74;
            }
            // A revision that exceeds the admitted active-instance capacity
            // must fail as a whole. In particular, the already committed
            // 100k revision remains the scene's authoritative state.
            auto oversized_instances = std::make_shared<std::vector<
                RenderClusterWireInstance>>(*stress_instances);
            oversized_instances->push_back((*stress_instances)[0u]);
            auto oversized = stress;
            oversized.revision = 3u;
            oversized.instance_count = static_cast<std::uint32_t>(
                oversized_instances->size());
            auto oversized_request = submitCluster(
                fixture.uploadClientForTest(),
                ops,
                oversized,
                oversized_instances);
            if (!oversized_request.valid())
                return 79;
            const auto oversized_reply = fixture.awaitUpload(
                std::move(oversized_request));
            if (oversized_reply.status != 3u)
            {
                std::fprintf(
                    stderr,
                    "100001-instance revision was not rejected as capacity: "
                    "status=%u\n",
                    oversized_reply.status);
                return 80;
            }
            const auto post_rejection_stats = fixture.awaitControl(
                control.stats({scene.scene_id}));
            if (post_rejection_stats.cluster_count != 1u ||
                post_rejection_stats.instance_count != count)
            {
                std::fprintf(
                    stderr,
                    "capacity rejection mutated active revision: clusters=%u "
                    "instances=%u\n",
                    post_rejection_stats.cluster_count,
                    post_rejection_stats.instance_count);
                return 81;
            }
            fixture.flush(2);
            const auto graph_after_revision_json = gpuTimingJson(
                fixture,
                scene.scene_id);
            const auto graph_compiles_after_revision =
                graph_after_revision_json
                ? jsonUnsigned(
                      *graph_after_revision_json,
                      "compile_successes")
                : std::nullopt;
            if (!graph_compiles_after_revision ||
                *graph_compiles_after_revision !=
                    *graph_compiles_before_revision)
            {
                std::fprintf(
                    stderr,
                    "100k in-place revision recompiled the graph: "
                    "before=%llu after=%llu\n",
                    static_cast<unsigned long long>(
                        *graph_compiles_before_revision),
                    static_cast<unsigned long long>(
                        graph_compiles_after_revision.value_or(0u)));
                return 78;
            }
            remove_revision = 4u;
        }
        if (fixture.awaitControl(control.remove({
                scene.scene_id,
                stress.id,
                remove_revision})).status != 0u)
        {
            return 57;
        }
        fixture.flush(2);
        if (count == 100'000u)
        {
            // Recreate the same scale immediately under a different cluster
            // identity. Slot generations must advance while the stable 32-bit
            // address space and already-published page addresses remain valid.
            stress.id = {};
            stress.id.bytes[0] = probe_id++;
            stress.revision = 1u;
            auto reuse_request = submitCluster(
                fixture.uploadClientForTest(),
                ops,
                stress,
                stress_instances);
            if (!reuse_request.valid() ||
                fixture.awaitUpload(std::move(reuse_request)).status != 0u)
            {
                return 75;
            }
            const auto reuse_stats = fixture.awaitControl(
                control.stats({scene.scene_id}));
            if (reuse_stats.cluster_count != 1u ||
                reuse_stats.instance_count != count)
            {
                return 76;
            }
            if (fixture.awaitControl(control.remove({
                    scene.scene_id,
                    stress.id,
                    2u})).status != 0u)
            {
                return 77;
            }
            fixture.flush(2);
        }
    }
    if (candidate_probe_milliseconds[0] >= 0.3)
    {
        std::fprintf(
            stderr,
            "Candidate 4k path %.6f ms exceeds 0.3 ms\n",
            candidate_probe_milliseconds[0]);
        return 59;
    }
    const auto unit_cost_10k = candidate_probe_milliseconds[1] /
        static_cast<double>(candidate_probe_counts[1]);
    const auto unit_cost_100k = candidate_probe_milliseconds[2] /
        static_cast<double>(candidate_probe_counts[2]);
    if (unit_cost_10k > 0.0 &&
        unit_cost_100k > unit_cost_10k * 1.25)
    {
        std::fprintf(
            stderr,
            "Candidate scaling regressed: 10k=%.6f ms 100k=%.6f ms\n",
            candidate_probe_milliseconds[1],
            candidate_probe_milliseconds[2]);
        return 60;
    }
    std::printf(
        "Candidate path ms: 4k=%.6f 10k=%.6f 100k=%.6f\n",
        candidate_probe_milliseconds[0],
        candidate_probe_milliseconds[1],
        candidate_probe_milliseconds[2]);

    UploadRenderClusterPayload parent = upload;
    parent.id = {};
    parent.id.bytes[0] = 0x60u;
    parent.revision = 1u;
    parent.child_count = 2u;
    parent.children[0].bytes[0] = 0x61u;
    parent.children[1].bytes[0] = 0x62u;
    upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, parent, instances);
    if (!upload_request.valid() ||
        fixture.awaitUpload(std::move(upload_request)).status != 0u)
    {
        return 20;
    }
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 1u ||
        stats.visible_cluster_count != 1u ||
        stats.visible_instance_count != 2u)
    {
        return 21;
    }
    std::array<std::size_t, 4u> hlod_red_pixels{};
    hlod_red_pixels[0] = countSemanticRedPixels();

    auto child = parent;
    child.id = parent.children[0];
    child.parent = parent.id;
    child.child_count = 0u;
    upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, child, instances);
    if (!upload_request.valid() ||
        fixture.awaitUpload(std::move(upload_request)).status != 0u)
    {
        return 22;
    }
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 2u ||
        stats.visible_cluster_count != 1u ||
        stats.visible_instance_count != 2u)
    {
        return 23;
    }
    hlod_red_pixels[1] = countSemanticRedPixels();

    child.id = parent.children[1];
    upload_request = submitCluster(
        fixture.uploadClientForTest(), ops, child, instances);
    if (!upload_request.valid() ||
        fixture.awaitUpload(std::move(upload_request)).status != 0u)
    {
        return 24;
    }
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 3u ||
        stats.visible_cluster_count != 2u ||
        stats.visible_instance_count != 4u)
    {
        return 25;
    }
    hlod_red_pixels[2] = countSemanticRedPixels();

    const auto child_removed = fixture.awaitControl(control.remove({
        scene.scene_id, parent.children[1], 2u}));
    if (child_removed.status != 0u)
        return 26;
    stats = fixture.awaitControl(control.stats({scene.scene_id}));
    if (stats.cluster_count != 2u ||
        stats.visible_cluster_count != 1u ||
        stats.visible_instance_count != 2u)
    {
        return 27;
    }
    hlod_red_pixels[3] = countSemanticRedPixels();
    const auto [minimum_hlod_pixels, maximum_hlod_pixels] =
        std::minmax_element(
            hlod_red_pixels.begin(),
            hlod_red_pixels.end());
    const auto allowed_hlod_delta = std::max<std::size_t>(
        64u,
        *minimum_hlod_pixels / 10u);
    std::printf(
        "HLOD semantic red pixels: parent=%zu partial=%zu fine=%zu fallback=%zu\n",
        hlod_red_pixels[0],
        hlod_red_pixels[1],
        hlod_red_pixels[2],
        hlod_red_pixels[3]);
    if (*minimum_hlod_pixels < 250u ||
        *maximum_hlod_pixels - *minimum_hlod_pixels > allowed_hlod_delta)
    {
        return 69;
    }
    (void)fixture.awaitControl(control.remove({
        scene.scene_id, parent.children[0], 2u}));
    (void)fixture.awaitControl(control.remove({
        scene.scene_id, parent.id, 2u}));

    // Complete semantic readback before switching this view to present. The
    // graph contains graphics + candidate compute work; recorder submission
    // order is GRAPHICS then COMPUTE (and TRANSFER when present), deliberately
    // leaving a non-graphics queue at the host-array tail. Repeated
    // remove/create cycles immediately reuse instance slots across more than
    // the FIF horizon, proving retirement joins queue identity/signals rather
    // than placing the slot fence on the array's last submission.
    fixture.control().bindSwapchain(scene.scene_id, scene.view);
    fixture.flush(4);
    for (std::uint8_t cycle = 0u; cycle < 6u; ++cycle)
    {
        stress.id = {};
        stress.id.bytes[0] = static_cast<std::uint8_t>(0x80u + cycle);
        stress.revision = 1u;
        auto present_instances = std::make_shared<std::vector<
            RenderClusterWireInstance>>(4'000u, (*instances)[0]);
        stress.instance_count = static_cast<std::uint32_t>(
            present_instances->size());
        upload_request = submitCluster(
            fixture.uploadClientForTest(),
            ops,
            stress,
            present_instances);
        if (!upload_request.valid() ||
            fixture.awaitUpload(std::move(upload_request)).status != 0u)
        {
            return 70;
        }
        fixture.flush(3);
        if (fixture.awaitControl(control.remove({
                scene.scene_id,
                stress.id,
                2u})).status != 0u)
        {
            return 71;
        }
        fixture.flush(2);
    }
    if (validation_errors.load(std::memory_order_acquire) != 0)
    {
        std::fprintf(
            stderr,
            "Vulkan validation errors: %d\n",
            validation_errors.load(std::memory_order_relaxed));
        return 68;
    }
    return 0;
}
