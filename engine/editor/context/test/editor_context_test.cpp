#ifdef NDEBUG
#undef NDEBUG
#endif

#include "PluginProbe.hpp"

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/context/detail/ToolsetTestAccess.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <stdexec/execution.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::copy_constructible<lux::editor::Toolset>);
static_assert(!std::move_constructible<lux::editor::Toolset>);
static_assert(!std::copy_constructible<lux::editor::EditorContext>);
static_assert(!std::move_constructible<lux::editor::EditorContext>);

namespace
{
    struct ToolProbeState final
    {
        int construction_count{};
        int stop_count{};
        std::vector<int> destruction_order;
    };

    template<int Id>
    class OrderedTool final
    {
    public:
        explicit OrderedTool(ToolProbeState& state) noexcept : state_(state)
        {
            ++state_.construction_count;
        }

        ~OrderedTool() noexcept
        {
            state_.destruction_order.push_back(Id);
        }

        void requestStop() noexcept
        {
            ++state_.stop_count;
        }

    private:
        ToolProbeState& state_;
    };

    template<std::size_t Id>
    struct FillerTool final
    {
        std::array<std::byte, (Id % 7U) + 1U> value{};
    };

    template<std::size_t... Ids>
    [[nodiscard]] bool installFillers(lux::editor::Toolset& toolset, std::index_sequence<Ids...>)
    {
        return (static_cast<bool>(toolset.install<FillerTool<Ids>>()) && ...);
    }

    struct ThrowingTool final
    {
        ThrowingTool()
        {
            throw 7;
        }
    };

    struct FrozenTool final
    {
    };

    struct StoppingTool final
    {
    };

    class EmptyProvider final : public lux::asset::IAssetProvider
    {
    public:
        [[nodiscard]] std::optional<lux::asset::AssetId> resolve(std::string_view) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] bool contains(const lux::asset::AssetId&) const override
        {
            return false;
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError>
        open(const lux::asset::AssetId&) const override
        {
            return lux::cxx::unexpected(lux::asset::EAssetStorageError::NOT_FOUND);
        }

        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>&) const override
        {
        }

        [[nodiscard]] std::optional<std::string> pathOf(const lux::asset::AssetId&) const override
        {
            return std::nullopt;
        }
    };

    class TestPane final : public lux::object::Object<TestPane, lux::ui::Pane>
    {
    public:
        explicit TestPane(lux::object::ObjectDispatcherRef dispatcher)
            : Object(
                  std::move(dispatcher),
                  lux::ui::PaneId{"editor.context.test"},
                  lux::ui::PaneTypeId{"editor.context.test"},
                  "Context Test"
              )
        {
        }

    protected:
        void draw(lux::ui::Frame&, lux::ui::PaneDrawContext&) override
        {
        }
    };

    struct CapabilityProbeState final
    {
        int stop_count{};
        int destruction_count{};
        bool stop_saw_capabilities{};
        bool destruction_saw_capabilities{};
    };

    class CapabilityProbeTool final
    {
    public:
        CapabilityProbeTool(
            CapabilityProbeState& state,
            lux::asset::AssetVfsView vfs,
            lux::ui::UISession& ui,
            const lux::scene::SceneMetaManager& scene_meta
        ) noexcept
            : state_(state), vfs_(std::move(vfs)), ui_(ui), scene_meta_(scene_meta)
        {
        }

        ~CapabilityProbeTool() noexcept
        {
            ++state_.destruction_count;
            state_.destruction_saw_capabilities = capabilitiesAlive();
        }

        void requestStop() noexcept
        {
            ++state_.stop_count;
            state_.stop_saw_capabilities = capabilitiesAlive();
        }

    private:
        [[nodiscard]] bool capabilitiesAlive() const noexcept
        {
            return static_cast<bool>(vfs_) && std::addressof(ui_.commandRouter()) != nullptr &&
                scene_meta_.components().empty();
        }

        CapabilityProbeState& state_;
        lux::asset::AssetVfsView vfs_;
        lux::ui::UISession& ui_;
        const lux::scene::SceneMetaManager& scene_meta_;
    };

    [[nodiscard]] lux::scene::SceneMetaManager emptySceneMeta()
    {
        auto schemas = lux::simulation::ecs::ComponentSchemaSet::build({});
        assert(schemas);
        auto scene_meta = lux::scene::SceneMetaManager::build({
            std::move(*schemas),
            lux::simulation::SimulationSystemRegistry{},
            {},
            {},
            {}
        });
        assert(scene_meta);
        return std::move(*scene_meta);
    }

    template<class Type>
    [[nodiscard]] Type worldId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] std::unique_ptr<lux::scene::Scene> makeEmptyScene(const lux::scene::SceneMetaManager& meta)
    {
        lux::world::WorldDescriptionBuilder world_builder;
        assert(world_builder.setIdentity(
            worldId<lux::world::WorldBundleId>(1U),
            worldId<lux::world::WorldBundleGeneration>(2U),
            "editor-selection-test"
        ));
        assert(world_builder.setPartitioner({lux::world::worldPartitionerId("test.none"), 1U}, 0U));
        auto world = std::move(world_builder).build();
        assert(world);
        auto world_owner = std::make_shared<lux::world::WorldDescription>(std::move(*world));

        lux::simulation::SimulationDescriptionBuilder simulation_builder;
        auto simulation = std::move(simulation_builder).build();
        assert(simulation);
        auto simulation_owner = std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation));

        lux::scene::SceneDescriptionBuilder scene_builder;
        scene_builder.setWorld(assetId(1U));
        scene_builder.setSimulation(assetId(2U));
        auto description = std::move(scene_builder).build();
        assert(description);
        auto scene_description = std::make_shared<lux::scene::SceneDescription>(std::move(*description));
        auto scene = lux::scene::Scene::create({
            std::move(scene_description),
            std::move(world_owner),
            std::move(simulation_owner),
            meta,
            {}
        });
        assert(scene);
        return std::move(*scene);
    }

    class SelectionObserver final : public lux::object::Object<SelectionObserver>
    {
    public:
        using Object::Object;

        void onChanged(const lux::editor::EditorSelectionChanged& change) noexcept
        {
            ++count;
            current = change.current;
        }

        int count{};
        lux::editor::EditorSelectionValue current{};
    };

    void testToolset()
    {
        ToolProbeState state;
        {
            lux::editor::Toolset toolset;
            auto first = toolset.install<OrderedTool<1>>(state);
            assert(first);
            auto* stable_address = std::addressof(first->get());
            assert(toolset.get<OrderedTool<1>>());
            assert(toolset.find<OrderedTool<1>>() == stable_address);

            const auto duplicate = toolset.install<OrderedTool<1>>(state);
            assert(!duplicate);
            assert(duplicate.error().code == lux::editor::EToolsetError::DUPLICATE_TOOL);
            assert(state.construction_count == 1);

            const auto missing = toolset.get<OrderedTool<2>>();
            assert(!missing);
            assert(missing.error().code == lux::editor::EToolsetError::MISSING_TOOL);
            assert(toolset.find<OrderedTool<2>>() == nullptr);

            assert(installFillers(toolset, std::make_index_sequence<64>{}));
            assert(toolset.find<OrderedTool<1>>() == stable_address);

            const auto throwing = toolset.install<ThrowingTool>();
            assert(!throwing);
            assert(throwing.error().code == lux::editor::EToolsetError::CONSTRUCTION_FAILURE);
            assert(toolset.find<ThrowingTool>() == nullptr);

            const auto first_opaque = lux::editor::detail::ToolsetTestAccess::installOpaque(
                toolset,
                lux::cxx::TypeToken{0x12345678U, "lux.editor.test.first_collision"}
            );
            assert(first_opaque);
            const auto collision = lux::editor::detail::ToolsetTestAccess::installOpaque(
                toolset,
                lux::cxx::TypeToken{0x12345678U, "lux.editor.test.second_collision"}
            );
            assert(!collision);
            assert(collision.error().code == lux::editor::EToolsetError::TYPE_COLLISION);

            auto second = toolset.install<OrderedTool<2>>(state);
            assert(second);
            const auto& const_toolset = std::as_const(toolset);
            const auto const_second = const_toolset.get<OrderedTool<2>>();
            assert(const_second);
            assert(std::addressof(const_second->get()) == toolset.find<OrderedTool<2>>());

            toolset.freeze();
            assert(toolset.frozen());
            const auto frozen = toolset.install<FrozenTool>();
            assert(!frozen);
            assert(frozen.error().code == lux::editor::EToolsetError::FROZEN);

            toolset.requestStop();
            toolset.requestStop();
            assert(toolset.stopping());
            assert(state.stop_count == 2);
            const auto stopping = toolset.install<StoppingTool>();
            assert(!stopping);
            assert(stopping.error().code == lux::editor::EToolsetError::STOPPING);
        }
        assert(state.destruction_order == std::vector<int>({2, 1}));
    }

    void testEditorContext()
    {
        CapabilityProbeState capability_state;
        lux::editor::test::PluginProbeState plugin_state;
        auto runtime_created = lux::process::ExecutionRuntime::create({2U, 8U, 8U, {8U}});
        assert(runtime_created);
        auto runtime = std::move(*runtime_created);
        lux::ui::UISession ui;
        auto scene_meta = emptySceneMeta();
        auto scene = makeEmptyScene(scene_meta);
        lux::asset::AssetVfs vfs;
        lux::editor::EditorSelection selection{ui.dispatcherRef()};
        {
            lux::process::TaskScope tasks;
            lux::editor::Toolset toolset;
            {
                lux::editor::EditorContext context{lux::editor::EditorContextCreateInfo{
                    toolset,
                    vfs.view(),
                    {},
                    runtime,
                    tasks,
                    selection,
                    ui,
                    scene_meta
                }};
                auto provider = std::make_shared<EmptyProvider>();
                const auto mount = vfs.mount({"/Game", std::move(provider), 0});
                assert(mount != lux::asset::kInvalidMountId);

                auto capability = context.toolchain().install<CapabilityProbeTool>(
                    capability_state,
                    context.vfs(),
                    context.ui(),
                    context.sceneMeta()
                );
                assert(capability);
                auto plugin = lux::editor::test::installPluginProbe(context.toolchain(), plugin_state);
                assert(plugin);
                assert(plugin->get().value() == 42);
                const auto duplicate_plugin = lux::editor::test::installPluginProbe(context.toolchain(), plugin_state);
                assert(!duplicate_plugin);
                assert(duplicate_plugin.error().code == lux::editor::EToolsetError::DUPLICATE_TOOL);
                context.toolchain().freeze();

                {
                    TestPane pane{context.ui().dispatcherRef()};
                    auto registration = context.ui().registerPane(pane);
                    assert(registration);
                }

                assert(vfs.mountCount() == 1U);
                assert(context.vfs());
                assert(!context.assetRead());
                assert(context.toolchain().find<CapabilityProbeTool>() != nullptr);
                assert(context.toolchain().find<lux::editor::test::PluginProbeTool>() != nullptr);
                assert(context.sceneMeta().components().empty());
                assert(std::addressof(context.execution()) == std::addressof(runtime));
                assert(std::addressof(context.tasks()) == std::addressof(tasks));
                assert(std::addressof(context.selection()) == std::addressof(selection));
                assert(std::addressof(context.ui()) == std::addressof(ui));

                SelectionObserver observer{ui.dispatcherRef()};
                auto connection = context.selection().observeScoped<lux::editor::EditorSelection::changed>(
                    [&observer](const lux::editor::EditorSelectionChanged& change) noexcept {
                        observer.onChanged(change);
                    }
                );
                assert(connection);
                const auto first_scene = context.selection().activate(*scene);
                assert(first_scene.valid());
                const auto entity = scene->registry().create();
                assert(context.selection().select(first_scene, entity));
                assert(observer.current.entity == entity);
                scene->registry().destroy(entity);
                assert(!context.selection().validate());
                assert(observer.current.entity == lux::simulation::ecs::NullEntity);
                const auto second_scene = context.selection().activate(*scene);
                assert(second_scene.valid() && second_scene != first_scene);
                assert(!context.selection().select(first_scene, lux::simulation::ecs::NullEntity));
                assert(!context.selection().deactivate(first_scene));
                assert(context.selection().deactivate(second_scene));
                assert(!context.selection().select(second_scene, lux::simulation::ecs::NullEntity));

                const auto& const_context = std::as_const(context);
                assert(std::addressof(const_context.toolchain()) == std::addressof(context.toolchain()));
                assert(const_context.vfs());
                assert(std::addressof(const_context.ui()) == std::addressof(context.ui()));
            }
            const auto closed = stdexec::sync_wait(tasks.close());
            assert(closed);
            toolset.requestStop();
        }

        runtime.requestStop();
        assert(runtime.join());

        assert(capability_state.stop_count == 1);
        assert(capability_state.destruction_count == 1);
        assert(capability_state.stop_saw_capabilities);
        assert(capability_state.destruction_saw_capabilities);
        assert(plugin_state.stop_count == 1);
        assert(plugin_state.destruction_count == 1);
    }
}

int main()
{
    testToolset();

    lux::meta::ReflectionRegistry::initRegistry();
    testEditorContext();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
