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
    camera.aspect_ratio = 2.0;
    math::Ray3d ray;
    auto projection = scene::cameraProjection(camera);
    assert(projection);
    math::screenToRay(100.0, 50.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.isApprox(-Eigen::Vector3d::UnitZ()) && std::abs(ray.origin.z() + 0.05) < 1e-10);
    math::screenToRay(0.0, 0.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.x() < 0 && ray.direction.y() > 0 && ray.direction.z() < 0);

    camera.projection = scene::OrthographicProjection{10, 0.1, 100};
    projection = scene::cameraProjection(camera);
    math::screenToRay(0.0, 0.0, 200.0, 100.0, Eigen::Matrix4d(projection->inverse()), ray);
    assert(ray.direction.isApprox(-Eigen::Vector3d::UnitZ()));
    assert(ray.origin.isApprox(Eigen::Vector3d{-10, 5, -0.1}));
    std::get<scene::OrthographicProjection>(camera.projection).vertical_extent = 0;
    assert(!scene::cameraProjection(camera));
    camera.projection = scene::PerspectiveProjection{};
    camera.view = {3, 7};
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
    assert(decoded.primary && decoded.view.isNull() && decoded.aspect_ratio == 1.0);

    render::FeatureCatalog catalog;
    render::FeatureFactory factory;
    factory.name = "StandardViewCamera";
    factory.descriptor = render::kViewCameraDescriptor;
    const std::array<render::TypeId, 2> operations{101, 102};
    catalog.add(factory, 1, operations);
    const auto bindings = scene::builtinRenderFeatureSceneBindings();
    const auto binding =
        std::ranges::find(bindings, factory.descriptor.type, &scene::RenderFeatureSceneBinding::feature);
    assert(binding != bindings.end());
    auto stage = binding->create_sync_stage({registry, {2, 1}, catalog, factory.descriptor.type, {1, 1}, 2048, {}});
    assert(stage);
    render::RenderProgram<> packet;
    render::RenderProgramBuilder<> builder(packet);
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(packet.commands.size() == 1 && packet.commands.front().type_id == operations[0]);
    const auto &record = packet.commands.front();
    const auto *update =
        reinterpret_cast<const render::ViewCameraUpdatePayload *>(packet.payload.data() + record.payload_offset);
    assert(update->view == camera.view && update->coordinate_page_size == 2048);
    const double recovered = double(update->render_origin.page_delta[0]) * 2048 + update->render_origin.local[0];
    assert(recovered == 1e12);
    (*stage)->discardPrepared();
    assert((*stage)->hasPendingChanges());
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    (*stage)->commitPrepared();
    assert(!(*stage)->hasPendingChanges());
    const auto retired_view = camera.view;
    registry.remove<scene::Camera>(entity);
    builder.begin();
    assert((*stage)->prepare(builder) == scene::ERenderSyncPrepareResult::PREPARED_COMMANDS);
    assert(packet.commands.size() == 1 && packet.commands.front().type_id == operations[1]);
    const auto *removed = reinterpret_cast<const render::ViewCameraRemovePayload *>(
        packet.payload.data() + packet.commands.front().payload_offset);
    assert(removed->view == retired_view);
    (*stage)->commitPrepared();
    std::puts(
        "PASS camera: perspective/orthographic NDC, invalid projection, codec resets View, extraction retry/removal");
}
