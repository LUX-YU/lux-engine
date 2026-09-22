#pragma once

#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

struct RenderAssociationChecks final
{
    struct Consumed final
    {
        std::shared_ptr<std::atomic_bool> flag;
        explicit Consumed(std::shared_ptr<std::atomic_bool> value) : flag(std::move(value))
        {
        }
        ~Consumed()
        {
            flag->store(true, std::memory_order_release);
        }
    };
    lux::render::RenderRuntime *runtime{};
    lux::render::LightOperationIds light;
    lux::render::MeshStackOperationIds mesh;
    lux::render::RenderSceneId scene_id;
    lux::render::UpsertMeshInstancePayload mesh_upsert;
    lux::render::RenderProgram<> program;
    std::shared_ptr<std::atomic_bool> consumed;
    lux::render::RenderRequest<lux::render::MeshStackStatsReply> mesh_stats;
    lux::render::RenderRequest<lux::render::LightStatsReply> light_stats;
    unsigned phase{};
    bool pending{}, correct{true};
    std::uint32_t initial_mesh{}, initial_light{};

    void begin(lux::editor::scene::SceneEditor &scene, lux::render::RenderRuntime &renderer)
    {
        assert(scene.renderScene());
        runtime = &renderer;
        scene_id = *scene.renderScene();
        mesh = runtime->features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
        light = runtime->features().ops<lux::render::LightOperationIds>("Light");
        assert(mesh.valid() && light.valid());
        const auto *resolved = static_cast<const lux::scene::ResolvedMeshResources *>(
            scene.component(scene.objects().front().object, lux::cxx::typeToken<lux::scene::ResolvedMeshResources>()));
        assert(resolved);
        mesh_upsert.scene_id = scene_id;
        mesh_upsert.entity = RenderSourceZero();
        mesh_upsert.mesh = resolved->mesh;
        mesh_upsert.material = resolved->material;
        const float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        mesh_upsert.transform = lux::render::makeTransientRenderSpatialTransform3D(identity);
        query();
    }
    static constexpr lux::render::RenderEntityId RenderSourceZero() noexcept
    {
        return {};
    }
    void query()
    {
        mesh_stats = lux::render::MeshStackControlClient(runtime->control()->get(), mesh).stats({scene_id});
        light_stats = lux::render::LightControlClient(runtime->control()->get(), light).stats({scene_id});
        assert(mesh_stats.valid() && light_stats.valid());
    }
    template <class F> void prepare(F fill)
    {
        assert(!pending);
        lux::render::RenderProgramSession::Builder builder(program);
        builder.begin();
        program.kind = lux::render::ERenderProgramKind::StateUpdate;
        fill(builder);
        consumed = std::make_shared<std::atomic_bool>(false);
        static_cast<void>(builder.emplaceAttachment<Consumed>(lux::render::attachment_types::OwnedObject, consumed));
        pending = true;
    }
    static void changeMesh(lux::editor::scene::SceneEditor &scene, double x)
    {
        using Transform = lux::simulation::ecs::Transform3D;
        // The fixed real fixture materializes its first mesh as ECS entity zero.
        const auto object = scene.objects().front().object;
        const auto *value = static_cast<const Transform *>(scene.component(object, lux::cxx::typeToken<Transform>()));
        assert(value);
        Eigen::Vector3d translation = value->translation;
        translation.x() = x;
        const auto field = [](auto &v) noexcept { return &v.translation; };
        assert(scene.setField<Transform>(*scene.writeTarget(object), "Transform3D.translation", "Translation", field,
                                         translation));
    }
    bool poll(lux::editor::scene::SceneEditor &scene)
    {
        using namespace lux::render;
        if (pending)
        {
            auto submitted = runtime->submit(program);
            assert(submitted);
            if (*submitted == EFrameSubmit::BACKPRESSURED)
            {
                return false;
            }
            pending = false;
            return false;
        }
        if (consumed && !consumed->load(std::memory_order_acquire))
        {
            return false;
        }
        const RenderEntityId source{}; // Zero is a valid source key.
        UpsertLightPayload upsert;
        upsert.scene_id = scene_id;
        upsert.entity = source;
        upsert.light_type = 1;
        if (phase == 0)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            initial_mesh = mesh_stats.tryResult()->get().alive_instances;
            initial_light = light_stats.tryResult()->get().point_lights;
            if (initial_mesh != 3 || initial_light != 1)
            {
                static unsigned attempts{};
                if (++attempts % 120 == 0)
                {
                    std::fprintf(stderr, "R waiting initial Mesh=%u Point=%u\n", initial_mesh, initial_light);
                }
                query();
                return false;
            }
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), upsert);
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(), RemoveLightPayload{scene_id, source, 0});
            });
            phase = 1;
        }
        else if (phase == 1)
        {
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert);
                builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert);
            });
            phase = 3;
        }
        else if (phase == 2)
        {
            // The document's normal owner poll has published the changed mesh.
            prepare([](auto &) {});
            phase = 3;
        }
        else if (phase == 3)
        {
            query();
            phase = 4;
        }
        else if (phase == 4)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto count = mesh_stats.tryResult()->get().alive_instances;
            std::printf("R01 remove Light then Mesh upsert: before=%u after=%u source=0 expected=%u\n", initial_mesh,
                        count, initial_mesh);
            correct = correct && count == initial_mesh;
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), upsert);
                builder.push(opcodes::CommandOp, mesh.id<RemoveMeshInstanceOp>(),
                             RemoveMeshInstancePayload{scene_id, source});
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), upsert);
            });
            phase = 5;
        }
        else if (phase == 5)
        {
            query();
            phase = 6;
        }
        else if (phase == 6)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto count = light_stats.tryResult()->get().point_lights;
            std::printf("R02 remove Mesh then Light upsert: before=%u after=%u expected=%u\n", initial_light, count,
                        initial_light + 1);
            correct = correct && count == initial_light + 1;
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(), RemoveLightPayload{scene_id, source, 0});
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(), RemoveLightPayload{scene_id, source, 0});
            });

            phase = 7;
        }
        else if (phase == 7)
        {
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert);
                builder.push(opcodes::CommandOp, mesh.id<RetireMeshInstanceOp>(),
                             RetireMeshInstancePayload{scene_id, source, 500, 1});
                builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert);
                builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert);
            });
            phase = 8;
        }
        else if (phase == 8)
        {
            query();
            phase = 9;
        }
        else if (phase == 9)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto counts = mesh_stats.tryResult()->get();
            std::printf("R03 old fade + new source: instances=%u transitioning=%u\n", counts.alive_instances,
                        counts.transitioning_instances);
            correct = correct && counts.alive_instances == initial_mesh + 1 && counts.transitioning_instances == 1;
            phase = 10;
            query();
        }
        else if (phase == 10)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto counts = mesh_stats.tryResult()->get();
            if (counts.transitioning_instances != 0)
            {
                query();
                return false;
            }
            correct = correct && counts.alive_instances == initial_mesh;
            prepare(
                [&](auto &builder) { builder.push(opcodes::CommandOp, mesh.id<UpsertMeshInstanceOp>(), mesh_upsert); });
            phase = 11;
        }
        else if (phase == 11)
        {
            query();
            phase = 12;
        }
        else if (phase == 12)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto counts = mesh_stats.tryResult()->get();
            std::printf("R03 old retirement leaves new association: instances=%u expected=%u\n", counts.alive_instances,
                        initial_mesh);
            correct = correct && counts.alive_instances == initial_mesh;
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), upsert);
                auto next_generation = upsert;
                next_generation.entity = static_cast<RenderEntityId>(std::uint64_t{1} << 32);
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), next_generation);
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(), RemoveLightPayload{scene_id, source, 0});
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(), RemoveLightPayload{scene_id, source, 0});
                builder.push(opcodes::CommandOp, light.id<UpsertLightOp>(), next_generation);
            });
            phase = 13;
        }
        else if (phase == 13)
        {
            query();
            phase = 14;
        }
        else if (phase == 14)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            const auto count = light_stats.tryResult()->get().point_lights;
            std::printf("R05 complete generation key: late old removal leaves point_lights=%u expected=%u\n", count,
                        initial_light + 1);
            correct = correct && count == initial_light + 1;
            prepare([&](auto &builder) {
                builder.push(opcodes::CommandOp, light.id<RemoveLightOp>(),
                             RemoveLightPayload{scene_id, static_cast<RenderEntityId>(std::uint64_t{1} << 32), 0});
            });
            phase = 15;
        }
        else if (phase == 15)
        {
            query();
            phase = 16;
        }
        else if (phase == 16)
        {
            if (!mesh_stats.isReady() || !light_stats.isReady())
            {
                return false;
            }
            assert(mesh_stats.tryResult() && light_stats.tryResult());
            correct = correct && light_stats.tryResult()->get().point_lights == initial_light;
            runtime = {};
            std::fflush(stdout);
            assert(correct);
            std::puts("PASS independent Mesh/Light source associations through real Render handlers");
            return true;
        }
        return false;
    }
};
