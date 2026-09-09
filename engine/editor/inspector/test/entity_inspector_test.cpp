#ifdef NDEBUG
#undef NDEBUG
#endif

#include "PluginComponent.hpp"

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/inspector/EntityInspector.hpp>
#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>
#include <lux/engine/editor/inspector/GeneratedFieldEditor.hpp>
#include <lux/engine/editor/inspector/InspectorReadOnlyContext.hpp>
#include <lux/engine/editor/inspector/SemanticFieldEditors.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <stdexec/execution.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    template <class Type> [[nodiscard]] Type uuidId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::scene::SceneMetaManager firstPartySceneMeta()
    {
        std::vector<lux::simulation::ecs::ComponentSchema> schemas;
        const auto append = [&schemas](std::span<const lux::simulation::ecs::ComponentSchema> values)
        { schemas.insert(schemas.end(), values.begin(), values.end()); };
        append(lux::simulation::ecs::transformComponentSchemas());
        append(lux::simulation::ecs::hierarchyComponentSchemas());
        append(lux::simulation::ecs::visualComponentSchemas());
        auto schema_set = lux::simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
        assert(schema_set);
        auto meta = lux::scene::SceneMetaManager::build(
            {std::move(*schema_set), lux::simulation::SimulationSystemRegistry{}, {}, {}, {}}
        );
        assert(meta);
        return std::move(*meta);
    }

    [[nodiscard]] std::unique_ptr<lux::scene::Scene> makeScene(const lux::scene::SceneMetaManager& meta)
    {
        lux::world::WorldDescriptionBuilder world_builder;
        assert(world_builder.setIdentity(
            uuidId<lux::world::WorldBundleId>(1U),
            uuidId<lux::world::WorldBundleGeneration>(2U),
            "entity-inspector-test"
        ));
        assert(world_builder.setPartitioner({lux::world::worldPartitionerId("test.none"), 1U}, 0U));
        auto world = std::move(world_builder).build();
        assert(world);

        lux::simulation::SimulationDescriptionBuilder simulation_builder;
        auto simulation = std::move(simulation_builder).build();
        assert(simulation);

        lux::scene::SceneDescriptionBuilder scene_builder;
        scene_builder.setWorld(assetId(1U));
        scene_builder.setSimulation(assetId(2U));
        auto description = std::move(scene_builder).build();
        assert(description);

        auto scene = lux::scene::Scene::create(
            {std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
             std::make_shared<lux::world::WorldDescription>(std::move(*world)),
             std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)),
             meta,
             {}}
        );
        assert(scene);
        return std::move(*scene);
    }

    struct TransformObserver final
    {
        void updated(lux::simulation::ecs::Registry&, lux::simulation::ecs::Entity) noexcept
        {
            ++update_count;
        }

        int update_count{};
    };

    bool forbiddenEdit(lux::simulation::ecs::Registry&, lux::simulation::ecs::Entity,
                       lux::editor::inspector::InspectorContext&) noexcept
    {
        assert(false && "read-only inspector called a writable binding");
        return false;
    }

    class ReadOnlyProbe final : public lux::object::Object<ReadOnlyProbe, lux::ui::Pane>
    {
    public:
        ReadOnlyProbe(lux::ui::UISession& ui, const lux::simulation::ecs::Registry& registry,
                      lux::simulation::ecs::Entity entity,
                      const lux::editor::inspector::ComponentEditorBindingTable& bindings)
            : Object(ui.dispatcherRef(), lux::ui::PaneId{"readonly-probe"},
                     lux::ui::PaneTypeId{"readonly-probe"}, "Read-only probe"),
              registry_(registry), entity_(entity), bindings_(bindings) {}
        std::size_t calls{};
    private:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext&) override
        {
            auto table = frame.table(lux::ui::TableSpec{lux::ui::WidgetIdView{"values"}, 2U});
            if (!table.visible())
                return;
            lux::editor::inspector::InspectorReadOnlyContext context{frame};
            for (const auto& binding : bindings_.all())
            {
                binding.draw_read_only(registry_, entity_, context);
                ++calls;
            }
        }
        const lux::simulation::ecs::Registry& registry_;
        lux::simulation::ecs::Entity entity_;
        const lux::editor::inspector::ComponentEditorBindingTable& bindings_;
    };
} // namespace

int main()
{
    namespace ecs = lux::simulation::ecs;
    namespace inspector = lux::editor::inspector;

    lux::meta::ReflectionRegistry::initRegistry();
    auto runtime_result = lux::process::ExecutionRuntime::create({2U, 8U, 8U, {8U}});
    assert(runtime_result);
    auto runtime = std::move(*runtime_result);
    lux::ui::UISession ui;
    auto meta = firstPartySceneMeta();
    auto scene = makeScene(meta);
    lux::asset::AssetVfs vfs;
    lux::editor::EditorSelection selection{ui.dispatcherRef()};
    lux::process::TaskScope tasks;
    lux::editor::Toolset toolset;
    lux::editor::EditorContext context{
        lux::editor::EditorContextCreateInfo{toolset, vfs.view(), {}, runtime, tasks, selection, ui, meta}
    };

    const auto scene_handle = selection.activate(*scene);
    auto& registry = scene->registry();
    const auto parent_a = registry.create();
    const auto parent_b = registry.create();
    const auto entity = registry.create();
    registry.emplace<ecs::Transform3D>(entity);
    registry.emplace<ecs::WorldTransform3D>(entity);
    registry.emplace<ecs::Mesh3D>(entity);
    assert(ecs::reparent(registry, entity, parent_a));
    assert(selection.select(scene_handle, entity));

    auto first_party = inspector::buildFirstPartyComponentEditorBindings();
    assert(first_party);
    auto incomplete_source = inspector::buildFirstPartyComponentEditorBindings();
    assert(incomplete_source);
    std::vector<inspector::ComponentEditorBinding> incomplete_bindings;
    for (const auto& binding : incomplete_source->all())
    {
        if (binding.component_type != lux::cxx::typeToken<ecs::Mesh3D>())
            incomplete_bindings.push_back(binding);
    }
    auto incomplete_table = inspector::ComponentEditorBindingTable::build(std::move(incomplete_bindings));
    assert(incomplete_table);
    inspector::EntityInspector pane{
        ui.dispatcherRef(), lux::ui::PaneId{"entity-inspector-test"}, context, std::move(*first_party)
    };
    inspector::EntityInspector incomplete_pane{
        ui.dispatcherRef(),
        lux::ui::PaneId{"entity-inspector-missing-binding-test"},
        context,
        std::move(*incomplete_table)
    };
    auto registration = ui.registerPane(pane);
    assert(registration);
    auto incomplete_registration = ui.registerPane(incomplete_pane);
    assert(incomplete_registration);
    {
        auto frame = ui.beginFrame({{1280.0F, 720.0F}, 1.0F / 60.0F});
        frame.drawPanes();
    }
    assert(pane.lastDrawStats().visible_components == 3U);
    assert(pane.lastDrawStats().missing_bindings == 0U);
    assert(incomplete_pane.lastDrawStats().visible_components == 3U);
    assert(incomplete_pane.lastDrawStats().missing_bindings == 1U);

    const auto apply_translation = inspector::applyPlainField<ecs::Transform3D, &ecs::Transform3D::translation>;
    const auto apply_light = inspector::applyPlainField<ecs::Light3D, &ecs::Light3D::value>;
    TransformObserver observer;
    registry.on_update<ecs::Transform3D>().connect<&TransformObserver::updated>(observer);
    auto& transform = registry.get<ecs::Transform3D>(entity);
    const auto before_translation = transform.translation;
    const Eigen::Vector3d after_translation{2.0, 3.0, 4.0};
    assert((pane.undoJournal().begin<ecs::Transform3D, Eigen::Vector3d>(
        selection.current(), "translation", before_translation, apply_translation
    )));
    assert((apply_translation(registry, entity, after_translation)));
    assert((pane.undoJournal().commit<ecs::Transform3D, Eigen::Vector3d>(
        selection.current(), "translation", after_translation
    )));
    assert(observer.update_count == 1);
    assert(pane.undoJournal().undo(context));
    assert(registry.get<ecs::Transform3D>(entity).translation == before_translation);
    assert(pane.undoJournal().redo(context));
    assert(registry.get<ecs::Transform3D>(entity).translation == after_translation);
    assert(observer.update_count == 3);

    {
        auto read_only_bindings = inspector::buildFirstPartyComponentEditorBindings();
        assert(read_only_bindings);
        std::vector<inspector::ComponentEditorBinding> safe_bindings;
        for (auto binding : read_only_bindings->all())
        {
            assert(binding.draw_read_only != nullptr);
            binding.draw = forbiddenEdit;
            safe_bindings.push_back(std::move(binding));
        }
        auto table = inspector::ComponentEditorBindingTable::build(std::move(safe_bindings));
        assert(table);
        const auto saved_transform = registry.get<ecs::Transform3D>(entity);
        const auto saved_visual = registry.get<ecs::Mesh3D>(entity);
        const auto saved_selection = selection.current();
        // Exercise generated callbacks directly as well: collapsed trees must not mask writes.
        ReadOnlyProbe probe{ui, registry, entity, *read_only_bindings};
        auto probe_registration = ui.registerPane(probe);
        assert(probe_registration);
        inspector::EntityInspector read_only{
            ui.dispatcherRef(), lux::ui::PaneId{"read-only-inspector"}, context,
            std::move(*table), inspector::EInspectorMode::READ_ONLY
        };
        auto read_registration = ui.registerPane(read_only);
        assert(read_registration);
        {
            auto frame = ui.beginFrame({{1280.0F, 720.0F}, 1.0F / 60.0F});
            frame.drawPanes();
        }
        assert(probe.calls == read_only_bindings->all().size());
        assert(registry.get<ecs::Transform3D>(entity).translation == saved_transform.translation);
        assert(registry.get<ecs::Transform3D>(entity).rotation == saved_transform.rotation);
        assert(registry.get<ecs::Transform3D>(entity).scale == saved_transform.scale);
        assert(registry.get<ecs::Mesh3D>(entity).value == saved_visual.value);
        assert(selection.current().entity == saved_selection.entity);
        assert(selection.current().scene == saved_selection.scene);
        assert(observer.update_count == 3);
        assert(!read_only.undoJournal().hasActiveGesture());
        assert(read_only.undoJournal().undoDepth() == 0);
    }

    pane.undoJournal().clear();
    auto& light = registry.emplace<ecs::Light3D>(entity);
    const auto before_light = light.value;
    auto after_light = before_light;
    after_light.type = lux::rdesc::ELightType::SPOT;
    std::string dynamic_field = "value";
    assert((pane.undoJournal().begin<ecs::Light3D, lux::rdesc::LightDescription>(
        selection.current(), dynamic_field, before_light, apply_light
    )));
    assert((apply_light(registry, entity, after_light)));
    dynamic_field.clear();
    dynamic_field.shrink_to_fit();
    const std::string commit_field = "value";
    assert((pane.undoJournal().commit<ecs::Light3D, lux::rdesc::LightDescription>(
        selection.current(), commit_field, after_light
    )));
    assert(!pane.undoJournal().hasActiveGesture());
    assert(pane.undoJournal().undoDepth() == 1U);
    assert(pane.undoJournal().undo(context));
    assert(registry.get<ecs::Light3D>(entity).value == before_light);
    assert(pane.undoJournal().redo(context));
    assert(registry.get<ecs::Light3D>(entity).value == after_light);

    const auto begin_result_1 = pane.undoJournal().begin<ecs::Parent, ecs::Entity>(
        selection.current(), "entity", parent_a, inspector::applyParentRelation
    );
    assert(begin_result_1);
    assert(inspector::applyParentRelation(registry, entity, parent_b));
    assert((pane.undoJournal().commit<ecs::Parent, ecs::Entity>(selection.current(), "entity", parent_b)));
    assert(registry.get<ecs::Parent>(entity).entity == parent_b);
    assert(pane.undoJournal().undo(context));
    assert(registry.get<ecs::Parent>(entity).entity == parent_a);
    assert(pane.undoJournal().redo(context));
    assert(registry.get<ecs::Parent>(entity).entity == parent_b);
    pane.undoJournal().clear();
    const auto begin_result_2 = pane.undoJournal().begin<ecs::Parent, ecs::Entity>(
        selection.current(), "entity", parent_b, inspector::applyParentRelation
    );
    assert(begin_result_2);
    assert(inspector::applyParentRelation(registry, entity, ecs::NullEntity));
    assert(!registry.all_of<ecs::Parent>(entity));
    assert((pane.undoJournal().commit<ecs::Parent, ecs::Entity>(selection.current(), "entity", ecs::NullEntity)));
    assert(pane.undoJournal().undo(context));
    assert(registry.get<ecs::Parent>(entity).entity == parent_b);
    assert(pane.undoJournal().redo(context));
    assert(!registry.all_of<ecs::Parent>(entity));
    assert(pane.undoJournal().undo(context));
    assert(registry.get<ecs::Parent>(entity).entity == parent_b);
    assert(!inspector::applyParentRelation(registry, entity, entity));
    assert(registry.get<ecs::Parent>(entity).entity == parent_b);

    pane.undoJournal().clear();
    assert((pane.undoJournal().begin<ecs::Transform3D, Eigen::Vector3d>(
        selection.current(), "translation", after_translation, apply_translation
    )));
    assert((pane.undoJournal().commit<ecs::Transform3D, Eigen::Vector3d>(
        selection.current(), "translation", after_translation
    )));
    registry.destroy(entity);
    assert(!pane.undoJournal().undo(context));
    assert(pane.undoJournal().poisoned());
    {
        auto frame = ui.beginFrame({{1280.0F, 720.0F}, 1.0F / 60.0F});
        frame.drawPanes();
    }
    assert(pane.lastDrawStats().stale_selection);
    assert(selection.current().entity == ecs::NullEntity);

    registration->reset();
    incomplete_registration->reset();
    assert(selection.deactivate(scene_handle));
    assert(stdexec::sync_wait(tasks.close()));
    runtime.requestStop();
    assert(runtime.join());
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
