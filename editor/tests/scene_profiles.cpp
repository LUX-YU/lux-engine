#include <lux/cxx/core/scope_exit.hpp>
#include "api_contract.hpp"
#include "support/ContextFixture.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <imgui.h>
#include <lux/engine/Engine.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/editor/BuiltinValueTypes.value_editor.hpp>
#include <lux/engine/editor/ComponentEditors3D.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/plugins/PluginManager.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/render/extensions/features/CapabilityGraph.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/scene/FixedPartitionStreamingSystem.hpp>
#include <lux/engine/scene/SceneContentResolution.hpp>
#include <lux/engine/scene/ScenePackageFile.hpp>
#include <lux/engine/scene/ScenePreparation.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingConfiguration.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;

namespace
{
    uuids::uuid_name_generator identity{*uuids::uuid::from_string("55677517-5438-46d1-967e-2977ad66961b")};

    struct EditorOnlyValue final
    {
        unsigned value = 0;
    };

    void checkAutonomousEngine()
    {
        auto application = engine::Engine::create({1, 16, {8}}, 16);
        assert(application);
        assert((*application)->step() && (*application)->close());
    }

    void checkGeneratedEditors(EditorContext& context)
    {
        auto* previous = ImGui::GetCurrentContext();
        auto* widgets = ImGui::CreateContext();
        auto& input = ImGui::GetIO();
        input.IniFilename = nullptr;
        input.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        input.DisplaySize = {800, 800};
        input.DeltaTime = 1.F / 60.F;
        unsigned char* pixels{};
        int width{}, height{};
        input.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        scene::ViewComponent camera;
        const auto* editor = context.componentEditor(cxx::make_basic_type_info<scene::ViewComponent>());
        assert(editor);
        const auto draw = [&]()
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({500, 650});
            ImGui::Begin("Generated camera fields");
            editor->draw(&camera);
            const auto position = ImGui::GetItemRectMin();
            ImGui::End();
            ImGui::Render();
            return ImVec2{position.x + 4, position.y + 4};
        };
        const auto primary = draw();
        assert(!camera.primary && std::holds_alternative<scene::PerspectiveProjection>(camera.projection));
        input.AddMousePosEvent(primary.x, primary.y);
        draw();
        input.AddMouseButtonEvent(0, true);
        draw();
        input.AddMouseButtonEvent(0, false);
        draw();
        assert(camera.primary); // The real generated checkbox edits the draft, without a Core UI field.
        const auto& projection = std::get<scene::PerspectiveProjection>(camera.projection);
        assert(projection.near_plane == 0.05 && projection.far_plane == 100000.0);

        ecs::Transform3D transform;
        ecs::Mesh3D mesh;
        ecs::Light3D light;
        ImGui::NewFrame();
        ImGui::Begin("Generated nested fields");
        const auto draw_value = [&]<class T>(T& value)
        {
            const auto* selected = context.componentEditor(cxx::make_basic_type_info<T>());
            assert(selected);
            ImGui::PushID(selected);
            selected->draw(&value);
            ImGui::PopID();
        };
        draw_value(transform);
        draw_value(mesh);
        draw_value(light);
        ImGui::End();
        ImGui::Render();
        assert(transform.translation.isZero() && mesh.value == rdesc::MeshVisualDescription{});
        assert(light.value == rdesc::LightDescription{});
        // Exercise the same generated configuration functions as the installed
        // SceneConfigurationEditor widgets. These edit values, never Registry state.
        render::RasterSurfaceConfiguration surface;
        const auto draw_control = [&](auto&& control)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({500, 650});
            ImGui::Begin("Configuration fields");
            control();
            const auto position = ImGui::GetItemRectMin();
            ImGui::End();
            ImGui::Render();
            return ImVec2{position.x + 4, position.y + 4};
        };
        const auto click = [&](ImVec2 position, auto&& draw)
        {
            input.AddMousePosEvent(position.x, position.y);
            draw();
            input.AddMouseButtonEvent(0, true);
            draw();
            input.AddMouseButtonEvent(0, false);
            draw();
        };
        const auto draw_surface = [&] { return draw_control([&] { editValue(ValueEditOptions{}, surface); }); };
        const auto surface_position = draw_surface();
        click(surface_position, draw_surface);
        assert(!surface.visibility);
        auto encoded_surface = render::encodeRasterSurfaceConfiguration(surface);
        assert(encoded_surface && !render::decodeRasterSurfaceConfiguration(encoded_surface->bytes)->visibility);

        const auto draw_readonly = [&]
        { return draw_control([&] { editField("locked", surface, {.read_only = true}); }); };
        click(draw_readonly(), draw_readonly);
        assert(!surface.visibility);
        render::ClusterGrid clustered;
        using VConfiguration = std::variant<render::RasterSurfaceConfiguration, render::MeshShadowsConfiguration>;
        VConfiguration alternatives;
        draw_control(
            [&]
            {
                editValue(ValueEditOptions{}, clustered);
                editValue({.label = "Professional configuration"}, alternatives);
            }
        );
        const auto key = [&](ImGuiKey code, auto&& draw)
        {
            input.AddKeyEvent(code, true);
            draw();
            input.AddKeyEvent(code, false);
            draw();
        };
        const auto draw_depth = [&]
        { return draw_control([&] { editValue({.label = "Distribution"}, clustered.depth); }); };
        click(draw_depth(), draw_depth);
        key(ImGuiKey_Home, draw_depth);
        key(ImGuiKey_Enter, draw_depth);
        assert(clustered.depth == render::EClusterDepth::LINEAR);
        ImVec2 variant_position;
        const auto draw_variant = [&]
        {
            return draw_control(
                [&]
                {
                    variant_position = ImGui::GetCursorScreenPos();
                    variant_position.x += 8;
                    variant_position.y += 8;
                    editValue({.label = "Professional configuration"}, alternatives);
                }
            );
        };
        draw_variant();
        click(variant_position, draw_variant);
        key(ImGuiKey_End, draw_variant);
        key(ImGuiKey_Enter, draw_variant);
        assert(std::holds_alternative<render::MeshShadowsConfiguration>(alternatives));
        render::DeferredLightingConfiguration lighting;
        lighting.clusters = clustered;
        auto encoded_cluster = render::encodeDeferredLightingConfiguration(lighting);
        assert(encoded_cluster && render::decodeDeferredLightingConfiguration(encoded_cluster->bytes)->clusters == clustered);
        auto& shadow = std::get<render::MeshShadowsConfiguration>(alternatives);
        shadow.resolution = 0;
        lighting.shadow = shadow;
        assert(!render::encodeDeferredLightingConfiguration(lighting));

        ImGui::DestroyContext(widgets);
        ImGui::SetCurrentContext(previous);
    }

    void checkCapabilityComposition()
    {
        using namespace lux::render;
        CapabilityGraphDescription source, consumer;
        source.ports = {
            {"surface",
             ECapabilityPortDirection::OUTPUT,
             "test.surface",
             CapabilityImageShape{rdesc::ETextureFormat::RGBA16_SFLOAT, 1, 1},
             "world"}
        };
        consumer.ports = {
            {"surface",
             ECapabilityPortDirection::INPUT,
             "test.surface",
             CapabilityImageShape{rdesc::ETextureFormat::RGBA16_SFLOAT, 1, 1},
             "world"},
            {"optional",
             ECapabilityPortDirection::INPUT,
             "test.surface",
             CapabilityImageShape{rdesc::ETextureFormat::RGBA16_SFLOAT, 1, 1},
             "world",
             false},
            {"color",
             ECapabilityPortDirection::OUTPUT,
             "test.color",
             CapabilityImageShape{rdesc::ETextureFormat::RGBA8_UNORM, 1, 1},
             "viewport"}
        };
        const std::array<std::string_view, 2> keys{"surface", "lighting"};
        const std::array<const CapabilityGraphDescription*, 2> declarations{&source, &consumer};
        const std::array outputs{CapabilityOutput{"lighting", "color"}};
        auto automatic = resolveCapabilityConnections(keys, declarations, {}, outputs);
        assert(
            automatic && automatic->connections.size() == 1 && automatic->order == std::vector<std::uint32_t>({0, 1})
        );
        const std::array<std::string_view, 3> ambiguous_keys{"first", "second", "lighting"};
        const std::array<const CapabilityGraphDescription*, 3> ambiguous_declarations{&source, &source, &consumer};
        assert(!resolveCapabilityConnections(ambiguous_keys, ambiguous_declarations, {}, outputs));
        const std::array explicit_link{CapabilityConnection{{"second", "surface"}, {"lighting", "surface"}}};
        assert(resolveCapabilityConnections(ambiguous_keys, ambiguous_declarations, explicit_link, outputs));
        consumer.ports[0].semantic = "test.surface.incompatible";
        assert(!resolveCapabilityConnections(keys, declarations, {}, outputs));
        consumer.ports[0].semantic = "test.surface";
        consumer.ports[0].coordinates = "view";
        assert(!resolveCapabilityConnections(keys, declarations, {}, outputs));
        consumer.ports[0].coordinates = "world";
        consumer.ports[0].shape = CapabilityImageShape{rdesc::ETextureFormat::D32_SFLOAT, 1, 1};
        assert(!resolveCapabilityConnections(keys, declarations, {}, outputs));

        const auto preset = deferredRenderComposition();
        assert(preset.contributions.size() == 2);
        auto bytes = encodeCapabilityComposition(preset);
        assert(bytes);
        auto decoded = decodeCapabilityComposition(*bytes);
        assert(decoded && *decoded == preset);
        bytes->pop_back();
        assert(!decodeCapabilityComposition(*bytes));
        auto invalid = preset;
        invalid.contributions[1].key = invalid.contributions[0].key;
        assert(!encodeCapabilityComposition(invalid));
        DeferredLightingConfiguration lighting;
        lighting.clusters.cells = {32, 18, 12};
        lighting.clusters.depth = EClusterDepth::LINEAR;
        auto light_bytes = encodeDeferredLightingConfiguration(lighting);
        assert(light_bytes && *decodeDeferredLightingConfiguration(light_bytes->bytes) == lighting);
        lighting.clusters.cells[1] = 0;
        assert(!encodeDeferredLightingConfiguration(lighting));
        lighting.clusters = {};
        lighting.shadow.resolution = 4;
        assert(!encodeDeferredLightingConfiguration(lighting));
        std::puts("PASS composition: unique/explicit ports, ambiguity, semantic version/space/shape, optional "
                  "isolation, portable preset/configuration");
    }

    struct PreparedValue final
    {
        unsigned value = 17;

        const unsigned& configuration() const noexcept
        {
            return value;
        }

        std::span<const system::SystemDataContract> dataInputs() const noexcept
        {
            return {};
        }
    };

    unsigned prepared_calls{}, resolved_calls{}, constructed_calls{}, configuration_decodes{};

    serialization::SerializationResult decodeProbe(std::span<const std::byte> input, void* output) noexcept
    {
        ++configuration_decodes;
        return serialization::makePortableValueCodec<unsigned>().decode(input, output);
    }

    struct ConstructionProbe final
    {
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.scene.preparation",
            .version = 1
        };

        ConstructionProbe() noexcept
        {
            ++constructed_calls;
        }
    };

    void checkColdConstruction(engine::Engine& engine, const scene::ScenePackage& source)
    {
        const std::array registrations{scene::SceneSystemRegistration{
            .type = system::systemTypeId(ConstructionProbe::Description.canonical_name),
            .cpp_type = cxx::make_basic_type_info<ConstructionProbe>(),
            .description = &ConstructionProbe::Description,
            .install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription input
                        ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure>
            {
                auto value = installer.emplaceSystem<ConstructionProbe>(input.instanceId());
                if (!value)
                {
                    return cxx::unexpected(value.error());
                }
                return {};
            },
            .resolve_data_inputs =
                +[](scene::SceneSystemDescription, std::span<const scene::ResolvedSceneDependency>) noexcept
                -> cxx::expected<std::vector<system::SystemDataContract>, scene::SceneSystemBuildFailure>
            {
                ++resolved_calls;
                return std::vector<system::SystemDataContract>{};
            },
            .prepare = +[](scene::SceneSystemDescription, std::span<const scene::ResolvedSceneDependency>) noexcept
                -> exec::task<cxx::expected<scene::PreparedSceneSystem, scene::SceneSystemBuildFailure>>
            {
                ++prepared_calls;
                auto value = std::make_unique<PreparedValue>();
                auto provider = scene::makeSceneDependency<PreparedValue>("prepared", "test.prepared", *value);
                co_return scene::PreparedSceneSystem::create(
                    std::move(value),
                    provider,
                    value->value,
                    value->dataInputs()
                );
            }
        }};
        scene::SceneDescriptionBuilder selected;
        assert(selected.addSystem({1}, "probe", registrations[0].type, 1, {}, 0, {}));
        auto description = std::move(selected).buildResolved();
        assert(description);
        auto description_source = std::make_shared<const scene::SceneDescription>(std::move(*description));
        auto prepared = stdexec::sync_wait(scene::prepareSceneSystems(description_source, registrations, {}));
        assert(prepared && std::get<0>(*prepared) && prepared_calls == 1 && constructed_calls == 0);
        scene::SceneDescriptionBuilder none;
        auto absent = std::move(none).buildResolved();
        assert(absent);
        auto unselected = stdexec::sync_wait(scene::prepareSceneSystems(
            std::make_shared<const scene::SceneDescription>(std::move(*absent)),
            registrations,
            {}
        ));
        assert(unselected && std::get<0>(*unselected)->empty() && prepared_calls == 1);

        PreparedValue first, second;
        const std::array services{
            scene::makeSceneDependency<PreparedValue>("first", "test.input", first),
            scene::makeSceneDependency<PreparedValue>("second", "test.input", second)
        };
        const std::array requirements{
            scene::SceneDependencyRequirement{"input", "test.input", cxx::make_basic_type_info<PreparedValue>(), false}
        };
        auto ambiguous = scene::resolveSceneDependencies(requirements, description_source->systemAt(0), services);
        assert(!ambiguous && ambiguous.error().code == scene::ESceneSystemBuildError::AMBIGUOUS_REQUIREMENT);
        scene::SceneDescriptionBuilder bound;
        assert(bound.addSystem({1}, "probe", registrations[0].type, 1, {}, 0, {}));
        assert(bound.bindRequirement({1}, "input", "second"));
        auto explicit_source = std::move(bound).buildResolved();
        assert(explicit_source);
        auto resolved = scene::resolveSceneDependencies(requirements, explicit_source->systemAt(0), services);
        assert(resolved && resolved->size() == 1 && resolved->front().value == &second);

        auto schemas = ecs::ComponentSchemaSet::build({});
        assert(schemas);
        simulation::SimulationSystemRegistry simulation;
        const auto provider = std::get<0>(*prepared)->front().provider();
        auto builder = engine.sceneRuntime().builder();
        builder.setDescription(description_source)
            .setWorld(source.world->sharedData())
            .setSimulation(source.simulation->sharedData())
            .setRegistrations(*schemas, simulation, registrations)
            .setProviders(std::span(&provider, 1));
        assert(builder.validate() && builder.validate() && resolved_calls == 1);
        builder.setPreparations(std::move(*std::get<0>(*prepared)));
        assert(
            builder.validate() && resolved_calls == 1
        ); // Same source: reuse preparation, no second decode/resolution.
        scene::SceneDescriptionBuilder other;
        assert(other.addSystem({1}, "probe", registrations[0].type, 1, {}, 0, {}));
        auto other_value = std::move(other).buildResolved();
        assert(other_value);
        builder.setDescription(std::make_shared<const scene::SceneDescription>(std::move(*other_value)));
        assert(builder.validate() && resolved_calls == 2); // Another source must qualify its provider agreement.
        builder.setDescription(description_source);
        assert(builder.validate() && resolved_calls == 3);
        auto replacement = registrations;
        builder.setRegistrations(*schemas, simulation, replacement);
        assert(builder.validate() && resolved_calls == 4);
        auto instance = builder.build();
        assert(instance && resolved_calls == 4 && constructed_calls == 1);
        auto retirement = *engine.sceneRuntime().retireInstance(*instance);
        while (!retirement.complete())
        {
            assert(engine.sceneRuntime().driveFrame(scene::ESceneDriveMode::ADVANCE));
        }
        auto contract = ConstructionProbe::Description;
        contract.configuration_schema_name = "test.prepared.configuration";
        contract.configuration_schema_version = 1;
        auto configured = registrations;
        configured[0].description = &contract;
        configured[0].configuration = serialization::makePortableValueCodec<unsigned>();
        configured[0].configuration.decode = &decodeProbe;
        configured[0].prepare =
            +[](scene::SceneSystemDescription input, std::span<const scene::ResolvedSceneDependency>) noexcept
            -> exec::task<cxx::expected<scene::PreparedSceneSystem, scene::SceneSystemBuildFailure>>
        {
            auto value = std::make_unique<PreparedValue>();
            auto decoded = decodeProbe(input.configurationPayload(), &value->value);
            if (!decoded)
            {
                co_return cxx::unexpected(
                    scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::CONFIGURATION_DECODE_FAILURE}
                );
            }
            auto provider = scene::makeSceneDependency<PreparedValue>("prepared", "test.prepared", *value);
            co_return scene::PreparedSceneSystem::create(std::move(value), provider, value->value, value->dataInputs());
        };
        configured[0].install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription input
                                 ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure>
        {
            auto config = installer.decodeConfiguration<unsigned>(input);
            assert(config && *config == 17);
            auto installed = installer.emplaceSystem<ConstructionProbe>(input.instanceId());
            return installed ? cxx::expected<void, scene::SceneSystemBuildFailure>{}
                             : cxx::unexpected(installed.error());
        };
        std::vector<std::byte> bytes;
        unsigned input_value = 17;
        assert(configured[0].configuration.encode(&input_value, bytes));
        scene::SceneDescriptionBuilder typed;
        assert(typed.addSystem({1}, "typed", configured[0].type, 1, contract.configuration_schema_name, 1, bytes));
        auto typed_source = std::make_shared<const scene::SceneDescription>(*std::move(typed).buildResolved());
        auto values = stdexec::sync_wait(scene::prepareSceneSystems(typed_source, configured, {}));
        assert(values && std::get<0>(*values) && configuration_decodes == 1);
        auto typed_builder = engine.sceneRuntime().builder();
        typed_builder.setDescription(typed_source)
            .setWorld(source.world->sharedData())
            .setSimulation(source.simulation->sharedData())
            .setRegistrations(*schemas, simulation, configured)
            .setPreparations(std::move(*std::get<0>(*values)));
        assert(typed_builder.validate() && typed_builder.validate() && configuration_decodes == 1);
        auto typed_instance = typed_builder.build();
        assert(typed_instance && configuration_decodes == 1);
        auto typed_retirement = *engine.sceneRuntime().retireInstance(*typed_instance);
        while (!typed_retirement.complete())
        {
            assert(engine.sceneRuntime().driveFrame(scene::ESceneDriveMode::ADVANCE));
        }
        auto new_registration = configured;
        new_registration[0].resolve_data_inputs =
            +[](scene::SceneSystemDescription, std::span<const scene::ResolvedSceneDependency>) noexcept
            -> cxx::expected<std::vector<system::SystemDataContract>, scene::SceneSystemBuildFailure>
        { return cxx::unexpected(scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::INVALID_DESCRIPTION}); };
        typed_builder.setRegistrations(*schemas, simulation, new_registration);
        assert(!typed_builder.validate()); // Must not reuse the earlier preparation's empty dataInputs().
        assert(configuration_decodes == 2);
        std::puts(
            "PASS cold construction: selected finite preparation, no unselected work, one resolution for validate/build"
        );
    }

    void checkContentDomains(const plugins::SceneRegistrations& registrations, const scene::ScenePackage& source)
    {
        const auto algorithm = std::ranges::find(
            registrations.partitions,
            std::string_view{"lux.spatial.builtin.single"},
            [](const auto& value) { return std::string_view(value.identity.id.name); }
        );
        assert(algorithm != registrations.partitions.end() && algorithm->build_empty);
        const std::array domains{
            world::WorldDomainSelection{"first", algorithm->identity},
            world::WorldDomainSelection{"second", algorithm->identity}
        };
        scene::SceneDescriptionBuilder selected;
        assert(selected.selectGeometry({{"lux.geometry.mesh3d", 1}, "first"}));
        auto description = std::move(selected).buildResolved();
        assert(description);
        auto package = scene::createScenePackage(
            asset::AssetId{identity("two-domains")},
            "Two domains",
            source.world->data().schemas(),
            source.simulation->sharedData(),
            *description,
            domains,
            registrations.partitions
        );
        assert(package && package->partitions.size() == 2);
        const auto& world = package->world->data();
        assert(world.domains().size() == 2 && world.partitionCount() == 2);
        assert(world.domainAt({0})->key == "first" && world.domainAt({1})->key == "second");
        assert(!world.domainAt({2}) && package->partitions[0]->id() != package->partitions[1]->id());
        auto resolution = scene::resolveSceneContent(
            registrations.allowed_content,
            description->geometries(),
            world,
            registrations.components,
            registrations.geometries,
            registrations.partitions
        );
        assert(resolution && resolution->partitions.size() == 2 && resolution->geometries.size() == 1);
        auto denied = registrations.allowed_content;
        denied.partitions.clear();
        assert(!scene::resolveSceneContent(
            denied,
            description->geometries(),
            world,
            registrations.components,
            registrations.geometries,
            registrations.partitions
        ));
        auto unknown = domains;
        unknown[1].partitioner.version = 999;
        assert(!scene::createScenePackage(
            asset::AssetId{identity("wrong-version")},
            "Wrong",
            world.schemas(),
            source.simulation->sharedData(),
            *description,
            unknown,
            registrations.partitions
        ));

        // Each representation refers to the same entity. An ambiguous primary domain
        // is rejected, never selected by declaration order.
        auto geometries = registrations.geometries;
        const auto mesh =
            std::ranges::find(geometries, std::string{"lux.geometry.mesh3d"}, &scene::GeometryDeclaration::name);
        assert(mesh != geometries.end());
        auto second = *mesh;
        second.name = "test.geometry.second_representation";
        geometries.push_back(std::move(second));
        auto allowed = registrations.allowed_content;
        allowed.geometries.push_back({geometries.back().name, 1});
        const std::array choices{
            scene::SceneGeometrySelection{{"lux.geometry.mesh3d", 1}, "first"},
            scene::SceneGeometrySelection{{geometries.back().name, 1}, "second"}
        };
        auto two = scene::resolveSceneContent(
            allowed,
            choices,
            world,
            registrations.components,
            geometries,
            registrations.partitions
        );
        assert(two);
        const std::array components{ecs::componentSchemaId("lux.ecs.Mesh3D")};
        auto ambiguous = scene::resolveObjectDomain(*two, world, geometries, components);
        assert(!ambiguous && ambiguous.error().code == scene::ESceneContentError::AMBIGUOUS_DOMAIN);
        assert(scene::resolveObjectDomain(*two, world, geometries, components, "second").value() == 1);

        // Actual asset codecs preserve domain intervals and selected geometry identities.
        auto encoded = scene::encodeScenePackage(*package, 16U * 1024U * 1024U);
        assert(encoded);
        auto owned = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        auto decoded = scene::decodeScenePackage(cxx::SharedBytes<>::fromOwner(owned, *owned));
        assert(decoded && decoded->world->data().domains().size() == 2);
        assert(decoded->scene->data().geometries().front().domain == "first");
        assert(decoded->partitions[1]->generation() == decoded->world->data().generation());
        assert(decoded->partitions[1]->partition().value == 1);
        std::puts(
            "PASS Editor content selection: two real domain layouts, codec roundtrip, exact versions, explicit primary"
        );
    }

    SceneCreateResult create2D(const SceneCreateInfo& info) noexcept
    {
        return scene::createScenePackage(
            info.id,
            info.name,
            {},
            std::make_shared<const simulation::SimulationDescription>(),
            {}
        );
    }

    void inspect3D(const scene::ScenePackage& package, asset::AssetId id)
    {
        assert(package.scene->id() == id && package.scene->data().world() == package.world->id());
        assert(package.scene->data().simulation() == package.simulation->id());
        assert(package.simulation->data().systemCount() == 0);
        assert(package.world->data().domains().front().partitioner.id.name == "lux.spatial.builtin.single");
        assert(package.world->data().partitionCount() == 1 && package.partitions.size() == 1);
        assert(package.partitions[0]->objectCount() == 0); // No authored editor camera/overlay.
        assert(package.volumes.size() == 1 && !package.volumes[0].empty());
        const auto schemas = package.world->data().schemas();
        assert(schemas.size() == 5);
        for (const auto name :
             {"lux.ecs.Parent", "lux.ecs.Transform3D", "lux.ecs.Mesh3D", "lux.ecs.Light3D", "lux.scene.Camera"})
        {
            assert(std::ranges::any_of(schemas, [&](const auto& schema) { return schema.name == name; }));
        }
        const auto& description = package.scene->data();
        assert(description.systemCount() == 5);
        assert(description.findSystem("mesh_query"));
        const auto transform = description.findSystem("transform");
        assert(transform && transform.type().name == "lux.scene.transform");
        scene::TransformSystemConfiguration transform_config;
        auto transform_codec = scene::transformSystemRegistration().configuration;
        assert(transform_codec.decode(transform.configurationPayload(), &transform_config));
        assert(transform_config.entity_capacity == 4096 && transform_config.max_commands == 32768);
        const auto loading = description.findSystem("world_loading");
        scene::WorldLoadingConfiguration world_config;
        assert(
            loading && scene::worldLoadingConfigurationCodec().decode(loading.configurationPayload(), &world_config)
        );
        assert(world_config.bootstrap.empty());
        const auto streaming = description.findSystem("streaming");
        scene::WorldStreamingDemand demand;
        assert(streaming && scene::worldStreamingDemandCodec().decode(streaming.configurationPayload(), &demand));
        assert(demand.required && demand.partitions == std::vector<partition::PartitionOrdinal>{{0}});
        const auto render = description.findSystem("render");
        assert(render && render.type().name == ::lux::engine::render::RenderSystem::Description.canonical_name);
        assert(
            render.configurationSchemaName() ==
            ::lux::engine::render::RenderSystem::Description.configuration_schema_name
        );
        ::lux::engine::render::RenderSystemConfiguration render_config;
        assert(::lux::engine::render::renderConfigurationCodec().decode(render.configurationPayload(), &render_config));
        assert(
            ::lux::engine::render::validRenderConfiguration(render_config) &&
            render_config.composition.contributions.size() == 2
        );
    }
} // namespace

int main(int argc, char** argv)
{
    checkAutonomousEngine();
    assert(argc == 4);
    checkCapabilityComposition();
    static_assert(!api_contract::PublicFreeze<SceneProfileRegistry>);
    auto& object_runtime = object::ObjectRuntime::instance();
    assert(object_runtime.isCurrent());
    auto engine = engine::Engine::create({1, 64, {32}}, 64);
    cxx::scope_exit close_engine([&]() noexcept { assert((*engine)->close()); });
    assert(engine && (*engine)->sceneRuntime().instanceCount() == 0);
    std::weak_ptr<int> provider;
    auto assembly = [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
    {
        static_assert(!api_contract::PublicProfileLookup<EditorComposition>);
        const ComponentEditor editor{
            {},
            cxx::make_basic_type_info<EditorOnlyValue>(),
            +[](void* value) noexcept { ++static_cast<EditorOnlyValue*>(value)->value; }
        };
        assert(composition.registerComponentEditor(editor));
        assert(!composition.registerComponentEditor(editor));
        assert(!composition.registerComponentEditor({}));
        assert(registerComponentEditors3D(composition));
        auto profile = sceneProfile3D();
        profile.id = "Bad Profile";
        assert(!composition.registerSceneProfile(std::move(profile)));
        profile = sceneProfile3D();
        profile.capabilities.push_back(profile.capabilities.front());
        assert(!composition.registerSceneProfile(std::move(profile)));
        assert(composition.registerSceneProfile(sceneProfile3D()));
        assert(!composition.registerSceneProfile(sceneProfile3D()));
        auto pin = std::make_shared<int>(42);
        provider = pin;
        return composition.registerSceneProfile({pin, "test.editor.scene.2d", "Test 2D", {"spatial.2d"}, &create2D});
    };
    ProjectManifest manifest{1, identity("project"), "Profiles"};
    SceneProfileRegistration three, two;
    auto inspect = [&](EditorContext& context) noexcept
    {
        EditorOnlyValue edited;
        const auto* editor = context.componentEditor(cxx::make_basic_type_info<EditorOnlyValue>());
        assert(editor && !context.componentEditor(cxx::make_basic_type_info<int>()));
        editor->draw(&edited);
        assert(edited.value == 1);
        checkGeneratedEditors(context);
        assert(context.sceneProfiles().profiles().size() == 2);
        static_assert(!api_contract::PublicProfileRegistration<SceneProfileRegistry>);
        assert(!context.sceneProfiles().find("test.missing"));
        three = context.sceneProfiles().find("lux.editor.scene.3d")->get();
        two = context.sceneProfiles().find("test.editor.scene.2d")->get();
    };
#if defined(LUX_INSTALLED_CONSUMER)
    fixture::withContext({manifest.name, std::filesystem::current_path()}, manifest, assembly, inspect);
#else
    auto context =
        fixture::createContext(**engine, {manifest.name, std::filesystem::current_path()}, manifest, assembly);
    assert(context);
    inspect(**context);
    context->reset();
#endif
    const auto scene_id = asset::AssetId{identity("scene")};
    plugins::SceneRegistrations registrations;
    {
        plugins::PluginCatalog catalog;
        assert(catalog.read(argv[1], argv[2]));
        const plugins::MetadataIdentity selected{"lux.builtin.scene_render", 2};
        auto plugins = plugins::PluginManager::create(std::move(catalog), std::span{&selected, 1});
        assert(plugins);
        auto read = plugins::readSceneRegistrations({}, plugins->libraries());
        assert(read);
        registrations = std::move(*read);
    } // Only the real registration leases now keep plugin codec callbacks alive.
    assert(!provider.expired()); // A copied profile safely owns its provider code.
    auto two_package = two.create({scene_id, "2D", registrations, {}});
    assert(two_package && two_package->scene->data().systemCount() == 0);
    two = {};
    assert(provider.expired());

    SceneCreateResult package = cxx::unexpected(scene::ScenePackageFailure{});
    std::thread worker([&] { package = three.create({scene_id, "3D", registrations, {}}); });
    worker.join();
    if (!package)
    {
        std::fprintf(stderr, "Profile failed: %u %s\n", unsigned(package.error().code), package.error().stage.c_str());
    }
    assert(package);
    inspect3D(*package, scene_id);
    checkContentDomains(registrations, *package);
    checkColdConstruction(**engine, *package);
    if (const auto* previous = std::getenv("LUX_TEST_PREVIOUS_SCENE"))
    {
        auto migrated = scene::readScenePackageFile(previous, 16U * 1024U * 1024U);
        assert(migrated && migrated->world->data().domains().size() == 1);
        assert(migrated->world->data().domains().front().key == world::kDefaultWorldDomain);
        assert(migrated->scene->data().geometries().empty());
        assert(scene::encodeScenePackage(*migrated, 16U * 1024U * 1024U));
        std::puts("PASS previous World/Scene v2 fixture decoded to default domain and encoded by current codecs");
    }
    assert((*engine)->sceneRuntime().instanceCount() == 0);

    plugins::SceneRegistrations missing;
    auto rejected = three.create({scene_id, "3D", missing, {}});
    assert(!rejected && rejected.error().stage == "3d profile schema");
    auto incomplete = registrations;
    std::erase_if(
        incomplete.scene_systems,
        [](const auto& entry)
        { return entry.type.name == ::lux::engine::render::RenderSystem::Description.canonical_name; }
    );
    rejected = three.create({scene_id, "3D", incomplete, {}});
    assert(!rejected && rejected.error().stage == "3d profile system");
    incomplete = registrations;
    incomplete.scene_systems.clear();
    rejected = three.create({scene_id, "3D", incomplete, {}});
    assert(!rejected && rejected.error().stage == "3d profile system");
    std::stop_source stopped;
    stopped.request_stop();
    rejected = three.create({scene_id, "3D", registrations, stopped.get_token()});
    assert(!rejected && rejected.error().code == scene::EScenePackageError::CANCELLED);

    // An unknown Pak member must remain byte-identical across actual scene file IO.
    auto opaque = std::make_shared<const std::vector<std::byte>>(
        std::initializer_list<std::byte>{std::byte{0x42}, std::byte{0x19}}
    );
    const auto opaque_id = asset::AssetId{identity("opaque")};
    package->package.entries.push_back(
        {{opaque_id, 0x55555555U, "Unknown"}, cxx::SharedBytes<>::fromOwner(opaque, *opaque)}
    );
    auto directory = std::filesystem::absolute(argv[3]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "profile.scene";
    std::filesystem::remove(file);
    constexpr std::size_t limit = 16U * 1024U * 1024U;
    auto written = scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::CREATE, limit);
    if (!written)
    {
        if (auto* error = std::get_if<platform::FileFailure>(&written.error()))
        {
            std::fprintf(stderr, "Scene file IO: %u %s\n", unsigned(error->code), error->system.message().c_str());
        }
        else
        {
            const auto& encoding = std::get<scene::ScenePackageFailure>(written.error());
            const auto* message = std::get_if<std::string>(&encoding.cause);
            std::fprintf(
                stderr,
                "Scene encoding: %u %s %s\n",
                unsigned(encoding.code),
                encoding.stage.c_str(),
                message ? message->c_str() : ""
            );
        }
    }
    assert(written);
    assert(!scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::CREATE, limit));
    auto loaded = scene::readScenePackageFile(file, limit);
    assert(loaded);
    inspect3D(*loaded, scene_id);
    auto roundtrip = scene::encodeScenePackage(*loaded, limit);
    assert(roundtrip);
    const auto found = std::ranges::find_if(
        loaded->package.entries,
        [&](const auto& entry) { return entry.metadata.id == opaque_id; }
    );
    assert(found != loaded->package.entries.end() && std::ranges::equal(found->bytes.view(), *opaque));
    assert(!scene::writeScenePackageAtomic(
        file,
        *two_package,
        platform::EFileWriteMode::REPLACE,
        limit,
        stopped.get_token()
    ));
    assert(!scene::writeScenePackageAtomic(file, *two_package, platform::EFileWriteMode::REPLACE, 1));
    loaded = scene::readScenePackageFile(file, limit);
    assert(loaded && scene::encodeScenePackage(*loaded, limit).value() == *roundtrip);
    assert(!scene::readScenePackageFile(file, 1));
    assert(!scene::readScenePackageFile(file, limit, stopped.get_token()));
    assert(scene::writeScenePackageAtomic(file, *two_package, platform::EFileWriteMode::REPLACE, limit));
    loaded = scene::readScenePackageFile(file, limit);
    assert(loaded && loaded->scene->data().systemCount() == 0);
    std::filesystem::remove(file);
    assert((*engine)->sceneRuntime().instanceCount() == 0);
    std::puts("PASS frozen profiles, independent 2D extension, real plugin 3D package/codecs/IO, no runtime Scene");
}
