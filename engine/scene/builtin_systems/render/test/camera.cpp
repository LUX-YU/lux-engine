#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/math/Picking.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>

int main()
{
    using namespace lux;
    using namespace simulation::ecs;
    scene::Camera camera;
    math::Ray3d ray;
    auto projection = scene::cameraProjection(camera, 2.0);
    assert(projection);
    math::screenToRay(100.0, 50.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.isApprox(-Eigen::Vector3d::UnitZ()) && std::abs(ray.origin.z() + 0.05) < 1e-10);
    math::screenToRay(0.0, 0.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.x() < 0 && ray.direction.y() > 0 && ray.direction.z() < 0);

    camera.projection = scene::OrthographicProjection{10, 0.1, 100};
    projection = scene::cameraProjection(camera, 2.0);
    math::screenToRay(0.0, 0.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.isApprox(-Eigen::Vector3d::UnitZ()));
    assert(ray.origin.isApprox(Eigen::Vector3d{-10, 5, -0.1}));
    std::get<scene::OrthographicProjection>(camera.projection).vertical_extent = 0;
    assert(!scene::cameraProjection(camera, 2.0));
    camera.projection = scene::PerspectiveProjection{};
    camera.primary = true;

    Registry registry;
    WorldEntityMap identities;
    const auto entity = registry.create();
    registry.emplace<scene::Camera>(entity, camera);
    auto &world = registry.emplace<WorldTransform3D>(entity);
    world.value.translation() = Eigen::Vector3d{1e12, 2, 3};
    auto view = scene::cameraView(world.value, world.value.translation());
    assert((view && view->block<3, 1>(0, 3).isZero()));
    const auto schemas = scene::sceneRenderComponentSchemas();
    const auto schema = std::ranges::find(schemas, cxx::typeToken<scene::Camera>(), &ComponentSchema::cpp_type);
    assert(schema != schemas.end());
    auto capture = schema->capture(registry, entity, schema->code_lifetime);
    assert(capture);
    auto bytes = capture->encode(identities, 4096);
    assert(bytes);
    const auto restored = registry.create();
    assert(schema->decode_emplace(registry, identities, restored, 1, *bytes));
    const auto &decoded = registry.get<scene::Camera>(restored);
    assert(decoded.primary);

    render::FeatureCatalog catalog;
    render::FeatureFactory factory;
    factory.name = "StandardViewCamera";
    factory.descriptor = render::kViewCameraDescriptor;
    const std::array<render::TypeId, 2> operations{101, 102};
    factory.operation_count = static_cast<std::uint32_t>(operations.size());
    assert(catalog.add(factory, 1, operations));
    const auto bindings = scene::builtinRenderFeatureSceneBindings();
    const auto binding =
        std::ranges::find(bindings, factory.descriptor.type, &scene::RenderFeatureSceneBinding::feature);
    assert(binding != bindings.end());
    scene::RenderViewAssociations views{{1}, {{{3, 7}, entity, {200, 100}}, {{4, 9}, entity, {100, 200}}}, 1};
    auto stage =
        binding->create_sync_stage({registry, {2, 1}, catalog, factory.descriptor.type, {1, 1}, 2048, {}, &views});
    assert(stage);
    render::RenderProgram<> packet;
    render::RenderProgramBuilder<> builder(packet);
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(packet.commands.size() == 1 && packet.commands.front().type_id == operations[0]);
    const auto &record = packet.commands.front();
    const auto *update =
        reinterpret_cast<const render::ViewCameraUpdatePayload *>(packet.payload.data() + record.payload_offset);
    assert(update[0].view == views.values[0].view && update[0].coordinate_page_size == 2048);
    assert(update[1].view == views.values[1].view);
    assert(std::abs(update[1].proj_matrix[0] / update[0].proj_matrix[0] - 4.0f) < 1e-6f);
    assert(update[0].proj_matrix[5] == update[1].proj_matrix[5]);
    const double recovered = double(update->render_origin.page_delta[0]) * 2048 + update->render_origin.local[0];
    assert(recovered == 1e12);
    (*stage)->discardPrepared();
    assert((*stage)->hasPendingChanges());
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    (*stage)->commitPrepared();
    assert(!(*stage)->hasPendingChanges());
    // Resize changes only association data, not the component or the other View.
    const auto first_projection = update[0].proj_matrix[0];
    views.values[1].extent = {200, 100};
    ++views.revision;
    assert((*stage)->hasPendingChanges());
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    update = reinterpret_cast<const render::ViewCameraUpdatePayload *>(packet.payload.data() +
                                                                       packet.commands.front().payload_offset);
    assert(update[0].proj_matrix[0] == first_projection && update[1].proj_matrix[0] == first_projection);
    (*stage)->commitPrepared();
    const auto retired_view = views.values[0].view;
    registry.remove<scene::Camera>(entity);
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(packet.commands.size() == 1 && packet.commands.front().type_id == operations[1]);
    const auto *removed = reinterpret_cast<const render::ViewCameraRemovePayload *>(
        packet.payload.data() + packet.commands.front().payload_offset);
    assert(removed[0].view == retired_view && removed[1].view == views.values[1].view);
    // Full Entity generation matters when the slot is reused by a different Camera.
    registry.destroy(entity);
    const auto replacement = registry.create();
    registry.emplace<scene::Camera>(replacement);
    registry.emplace<WorldTransform3D>(replacement);
    assert(replacement != entity);
    (*stage)->commitPrepared();
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_NO_COMMANDS);
    (*stage)->commitPrepared();
    std::puts("PASS camera: perspective/orthographic NDC, invalid projection, codec, one camera/two independent "
              "extents, extraction retry/removal/generation");
}
