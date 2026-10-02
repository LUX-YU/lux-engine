#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <cassert>
#include <iostream>

namespace
{
    struct PublicationGate final
    {
        inline static bool blocked{};
        inline static constexpr std::string_view worlds[]{"*"};
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "test.editing.publication_gate",
            .version = 1,
            .supported_world_types = worlds
        };
    };
    lux::scene::SceneSystemRegistration gateRegistration()
    {
        using namespace lux;
        return {
            .type = system::systemTypeId(PublicationGate::Description.canonical_name),
            .cpp_type = cxx::typeToken<PublicationGate>(),
            .description = &PublicationGate::Description,
            .install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure> {
                const auto gate = installer.emplaceSystem<PublicationGate>(description.instanceId());
                if (!gate)
                    return cxx::unexpected(gate.error());
                return installer.addPublicationTask<PublicationGate>(
                    description.instanceId(),
                    [](PublicationGate&) noexcept -> scene::SceneStageResult {
                        return PublicationGate::blocked ? scene::ESceneProgress::PENDING
                                                        : scene::ESceneProgress::COMPLETE;
                    }
                );
            }
        };
    }
}

int main()
{
    using namespace lux;
    namespace ecs = simulation::ecs;
    auto schemas = ecs::ComponentSchemaSet::build(ecs::transformComponentSchemas(), {});
    assert(schemas);
    simulation::SimulationSystemRegistry systems;
    scene::SceneDescriptionBuilder builder;
    const std::array registrations{gateRegistration()};
    assert(builder.addSystem({1}, "gate", registrations.front().type, 1, {}, 0));
    auto description = std::move(builder).buildResolved();
    assert(description);
    const auto shared = std::make_shared<const scene::SceneDescription>(std::move(*description));
    auto execution = process::ExecutionRuntime::create({1, 32, 32, {16}});
    assert(execution);
    auto runtime = scene::SceneRuntime::create(*execution, {0, 1024});
    assert(runtime);
    auto create = [&] {
        return (*runtime)
            ->builder()
            .setDescription(shared)
            .setWorld(std::make_shared<const world::WorldDescription>())
            .setSimulation(std::make_shared<const simulation::SimulationDescription>())
            .setRegistrations(*schemas, systems, registrations)
            .build();
    };
    auto created = create();
    assert(created);
    const auto instance = created->id();
    assert((*runtime)->pauseSimulation(instance));
    auto& registry = (*runtime)->borrowInstance(instance)->get();
    const auto entity = registry.create();
    registry.emplace<ecs::Transform3D>(entity);
    assert((*runtime)->driveFrame());
    auto history = editor::editing::EditHistory::create({{128, 1048576, 1048576, 256}, {}});
    assert(history);
    editor::scene::SceneEditing editing(**runtime, instance, *schemas, **history);
    const auto empty_entity = registry.create();
    const auto empty_reference = empty_entity;
    const auto transform_type = cxx::typeToken<ecs::Transform3D>();
    assert(schemas->find(transform_type)->create);
    assert(!schemas->find(cxx::typeToken<ecs::WorldTransform3D>())->create);
    const auto add_target = editing.writeTarget(empty_reference);
    assert(add_target && editing.canAddComponent(*add_target, transform_type));
    assert(!editing.addComponent(*add_target, cxx::typeToken<ecs::WorldTransform3D>()));
    assert(editing.addComponent(*add_target, transform_type));
    assert(registry.get<ecs::Transform3D>(empty_entity).translation.isZero());
    assert(!editing.addComponent(*editing.writeTarget(empty_reference), transform_type));
    assert(editor::editing::EditExecutor{}.undo(**history));
    assert(!registry.all_of<ecs::Transform3D>(empty_entity));
    assert(editing.componentChanges().empty());
    assert(editor::editing::EditExecutor{}.redo(**history));
    assert(registry.get<ecs::Transform3D>(empty_entity).scale == Eigen::Vector3d::Ones());
    assert(editor::editing::EditExecutor{}.undo(**history));
    assert(!editing.addComponent(*add_target, transform_type)); // Same state, different revision.
    const auto reference = entity;
    assert(editing.component(reference, cxx::typeToken<ecs::Transform3D>()));
    assert(editing.componentChanges().empty()); // Merely inspecting existing components creates no version table.
    const auto before = (*history)->view()->snapshot.current;
    const auto translation = [](auto& value) { return &value.translation; };
    auto target = editing.writeTarget(reference);
    assert(target);
    assert(editing.setField<ecs::Transform3D>(*target, "translation", "Move", translation, Eigen::Vector3d{1, 2, 3}));
    assert(registry.get<ecs::Transform3D>(entity).translation == Eigen::Vector3d(1, 2, 3));
    assert(editing.componentChanges().size() == 1);
    const auto edit_version = editing.componentVersion(reference, cxx::typeToken<ecs::Transform3D>());
    assert(edit_version != 0);
    assert(editor::editing::EditExecutor{}.undo(**history));
    assert(editing.componentVersion(reference, cxx::typeToken<ecs::Transform3D>()) > edit_version);
    assert(registry.get<ecs::Transform3D>(entity).translation.isZero());
    assert((*history)->view()->snapshot.current == before);
    assert(editor::editing::EditExecutor{}.redo(**history));
    PublicationGate::blocked = true;
    assert((*runtime)->resumeSimulation(instance));
    assert((*runtime)->driveFrame());
    assert(!(*runtime)->borrowInstance(instance));
    const auto blocked_undo = editor::editing::EditExecutor{}.undo(**history);
    assert(!blocked_undo && blocked_undo.error().code == editor::editing::EEditError::BUSY);
    assert(registry.get<ecs::Transform3D>(entity).translation == Eigen::Vector3d(1, 2, 3));
    assert((*runtime)->pauseSimulation(instance));
    PublicationGate::blocked = false;
    assert((*runtime)->driveFrame());
    assert((*runtime)->borrowInstance(instance));
    auto token = editing.beginFieldEdit<ecs::Transform3D, Eigen::Vector3d>(
        *editing.writeTarget(reference),
        "test",
        "translation",
        "Move",
        translation
    );
    assert(token && editing.fieldEditWritable(*token));
    assert(editing.finishFieldEdit(*token));
    registry.destroy(entity);
    const auto replacement = registry.create();
    registry.emplace<ecs::Transform3D>(replacement);
    assert(!editing.component(reference, cxx::typeToken<ecs::Transform3D>()));
    assert(!editing.writeTarget(reference));
    auto second = create();
    assert(second && second->id() != instance);
    editor::scene::SceneEditing second_editing(**runtime, second->id(), *schemas, **history);
    const auto second_entity = (*runtime)->borrowInstance(second->id())->get().create();
    (*runtime)->borrowInstance(second->id())->get().emplace<ecs::Transform3D>(second_entity);
    auto wrong_scene = *target;
    wrong_scene.entity = second_entity;
    const auto stale =
        second_editing
            .setField<ecs::Transform3D>(wrong_scene, "translation", "Move", translation, Eigen::Vector3d{8, 9, 10});
    assert(!stale && stale.error().code == editor::editing::EEditError::STALE_TARGET);
    assert((*runtime)->borrowInstance(second->id())->get().get<ecs::Transform3D>(second_entity).translation.isZero());
    assert((*runtime)->retireInstance(instance));
    assert(!editing.component(replacement, transform_type));
    const auto expired = editing.writeTarget(replacement);
    assert(!expired && expired.error().code == editor::editing::EEditError::STALE_TARGET);
    assert(editor::editing::EditExecutor{}.close(**history));
    editing.close();
    assert(!editing.fieldEditWritable(*token));
    std::cout
        << "PASS shared editing: no SceneEditor, Renderer or loader; existing entities, undo/redo, stale identities\n";
}
