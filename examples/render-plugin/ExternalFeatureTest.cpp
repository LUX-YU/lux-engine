#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include "ExternalFeature.hpp"
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#undef assert
#define assert(expression)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression);                                 \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (false)

int main(int argc, char** argv)
{
    assert(argc == 3);
    using namespace lux;
    using namespace std::chrono_literals;
    project::PluginCatalog metadata;
    assert(metadata.read(argv[1], argv[2]));
    const auto order = metadata.loadOrder("sample.render.triangle");
    assert(order);
    std::vector<std::shared_ptr<const project::PluginLibrary>> libraries;
    for (const auto* description : *order)
    {
        auto library = project::PluginLibrary::load(*description, libraries);
        if (!library)
            std::fprintf(
                stderr,
                "plugin rejected: %s / %s (%u)\n",
                description->identity.id.c_str(),
                library.error().subject.c_str(),
                unsigned(library.error().code)
            );
        assert(library);
        libraries.push_back(std::move(*library));
    }
    auto library = libraries.back();
    std::weak_ptr<const void> module_code = library->runtimeCode();
    auto rendering = project::readPluginRendering(*library, libraries);
    assert(rendering && rendering->features.size() == 1 && rendering->bindings.size() == 1);
    render::RendererConfig renderer;
    renderer.validation = true;
    renderer.control_capacity = 2;
    auto created = render::RenderRuntime::create(renderer, [](auto severity, auto message) {
        if (severity == 2)
            std::fprintf(stderr, "%.*s\n", int(message.size()), message.data());
    });
    assert(created);
    auto runtime = std::move(*created);
    auto execution = lux::process::ExecutionRuntime::create({1, 64, 64, {64}});
    assert(execution);
    lux::process::TaskScope tasks{*execution};
    auto made_resources = lux::scene::RenderResources::create(*runtime, tasks, execution->cpu());
    assert(made_resources);
    auto resources = std::move(*made_resources);

    const auto pump = [&] {
        std::size_t controls = 2, programs = 1;
        assert(runtime->collectCompletions(8));
        assert(runtime->submitPending(controls, programs));
        assert(execution->collectCompletions());
    };
    const auto until = [&](auto predicate) {
        const auto limit = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < limit)
        {
            // A predicate can admit a packet. Success must not submit it again.
            if (predicate())
                return;
            pump();
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "The requested completion did not arrive");
    };
    auto old_scene = resources->requestScene({.name = "pre-existing Scene"}, {});
    assert(old_scene);
    until([&] { return resources->sceneReceipt(*old_scene).status().state == scene::ESceneResourceState::READY; });
    const auto old_id = resources->sceneReceipt(*old_scene).status().scene;
    auto registration = rendering->features.front();
    const auto registration_started = std::chrono::steady_clock::now();
    assert(runtime->beginFeatureRegistration({registration}));
    until([&] { return runtime->featureRegistrationStatus().state == render::EFeatureRegistrationState::READY; });
    assert(!runtime->features().find(sample_ext::kFeature)); // Private until adoption.
    const auto registration_ready = std::chrono::steady_clock::now();
    assert(runtime->commitFeatureRegistration());
    const auto adoption_finished = std::chrono::steady_clock::now();
    std::printf(
        "Feature cold path: backend registration+poll=%.1f us, Main adoption=%.1f us\n",
        std::chrono::duration<double, std::micro>(registration_ready - registration_started).count(),
        std::chrono::duration<double, std::micro>(adoption_finished - registration_ready).count()
    );
    const auto* stable_entry = runtime->features().find(sample_ext::kFeature);
    assert(stable_entry && resources->sceneReceipt(*old_scene).status().scene == old_id);
    const auto stable_operation = runtime->features().paramSetOp("SampleExternal");
    auto& control = runtime->control()->get();
    // More outstanding replies than transport slots: every accepted request completes.
    std::vector<render::TRenderRequest<render::FeatureTypeRegisteredReply>> duplicates;
    for (unsigned index{}; index < 8; ++index)
        duplicates.push_back(control.registerFeatureType(registration.factory, registration.code_lifetime));
    for (auto& request : duplicates)
    {
        until([&] { return request.isReady(); });
        const auto reply = request.tryResult();
        assert(reply && reply->get().error.ok() && reply->get().status == 1);
        assert(reply->get().feature_type_id == stable_entry->feature_type_id);
        assert(reply->get().op_count == stable_entry->op_count && reply->get().ops[0] == stable_operation);
        auto release = control.unregisterFeatureType(reply->get().feature_type_id);
        until([&] { return release.isReady(); });
        assert(release.tryResult() && release.tryResult()->get().error.ok());
    }
    auto incompatible = registration.factory;
    incompatible.param_set_op_index = -1;
    auto collision = control.registerFeatureType(incompatible, registration.code_lifetime);
    until([&] { return collision.isReady(); });
    assert(
        collision.tryResult() &&
        collision.tryResult()->get().error.type ==
            render::renderError<render::err::feature::TypeIdCollision>(sample_ext::kFeature).type
    );
    incompatible = registration.factory;
    incompatible.descriptor.canonical_name = "sample.name.collision";
    incompatible.descriptor.type = render::featureId(incompatible.descriptor.canonical_name);
    collision = control.registerFeatureType(incompatible, registration.code_lifetime);
    until([&] { return collision.isReady(); });
    assert(
        collision.tryResult() && collision.tryResult()->get().error.type ==
                                     render::renderError<render::err::feature::FeatureNameCollision>(0).type
    );

    // Partial backend admission rolls back; the original catalog and live Scene survive.
    auto first = registration;
    first.factory.name = "RollbackFirst";
    first.factory.descriptor.canonical_name = "sample.rollback.first";
    first.factory.descriptor.type = render::featureId("sample.rollback.first");
    auto failing = registration;
    failing.factory.name = "RollbackFailure";
    failing.factory.descriptor.canonical_name = "sample.rollback.failure";
    failing.factory.descriptor.type = render::featureId("sample.rollback.failure");
    failing.factory.register_ops_fn = +[](void*, render::TypeId*, std::uint32_t) -> render::Expected<std::uint32_t> {
        return render::renderFailure<render::err::feature::InvalidRegistration>();
    };
    assert(runtime->beginFeatureRegistration({first, failing}));
    until([&] { return runtime->featureRegistrationStatus().state == render::EFeatureRegistrationState::FAILED; });
    assert(!runtime->features().find("RollbackFirst"));
    assert(runtime->beginFeatureRegistration({first}));
    pump(); // A Control request may already have been accepted.
    assert(runtime->cancelFeatureRegistration());
    until([&] { return runtime->featureRegistrationStatus().state == render::EFeatureRegistrationState::CANCELLED; });
    assert(!runtime->features().find("RollbackFirst"));
    assert(runtime->beginFeatureRegistration({first}));
    until([&] { return runtime->featureRegistrationStatus().state == render::EFeatureRegistrationState::READY; });
    assert(runtime->commitFeatureRegistration());
    assert(runtime->features().find(sample_ext::kFeature) == stable_entry);
    assert(runtime->features().paramSetOp("SampleExternal") == stable_operation);
    auto too_many = failing;
    too_many.factory.operation_count = 17;
    assert(!runtime->beginFeatureRegistration({too_many}));

    // Portable configuration survives a codec round trip before creating the actual RenderSystem.
    sample_ext::Tint red;
    std::vector<std::byte> portable;
    assert(registration.configuration.portable.encode(&red, portable));
    sample_ext::Tint decoded;
    assert(registration.configuration.portable.decode(portable, &decoded));
    assert(decoded.red == 1 && decoded.green == 0);
    const auto invalid_wire = registration.factory.create_fn(nullptr, &red, sizeof(red) - 1);
    assert(
        !invalid_wire &&
        invalid_wire.error().type ==
            render::renderError<render::err::comm::PayloadSizeMismatch>(sizeof(red), sizeof(red) - 1).type
    );
    scene::RenderSystemConfiguration configuration;
    configuration.features.push_back(
        {sample_ext::kFeature,
         portable,
         std::string(registration.configuration.schema),
         registration.configuration.schema_version}
    );
    const auto render_system = scene::builtinRenderSystemRegistration();
    std::vector<std::byte> configuration_bytes;
    assert(render_system.configuration.encode(&configuration, configuration_bytes));
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem(
        {1},
        "render",
        render_system.type,
        render_system.description->version,
        render_system.description->configuration_schema_name,
        render_system.description->configuration_schema_version,
        configuration_bytes
    ));
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto components =
        simulation::ecs::ComponentSchemaSet::build({library->components().begin(), library->components().end()});
    assert(components);
    simulation::SimulationSystemRegistry systems;
    const std::array scene_systems{render_system};
    scene::RenderFeatureSceneBindings bindings = rendering->bindings;
    rendering->features.clear();
    std::array providers{
        scene::makeSceneCapabilityProvider<scene::RenderResources>("resources", "lux.render.resources", *resources),
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *runtime),
        scene::makeSceneCapabilityProvider<scene::RenderFeatureSceneBindings>(
            "bindings",
            "lux.render.scene_bindings",
            bindings
        )
    };
    auto made_scenes = scene::SceneRuntime::create(*execution, {0, 128});
    assert(made_scenes);
    auto scenes = std::move(*made_scenes);
    auto build_scene = [&](std::shared_ptr<const scene::SceneDescription> input) {
        return scenes->builder()
            .setDescription(std::move(input))
            .setWorld(std::make_shared<const world::WorldDescription>())
            .setSimulation(std::make_shared<const simulation::SimulationDescription>())
            .setRegistrations(*components, systems, scene_systems)
            .setProviders(providers)
            .build();
    };
    for (unsigned invalid{}; invalid < 3; ++invalid)
    {
        auto candidate = configuration;
        if (invalid == 0)
            candidate.features.front().configuration_schema = "sample.wrong.schema";
        if (invalid == 1)
            ++candidate.features.front().configuration_version;
        if (invalid == 2)
            candidate.features.front().configuration.pop_back();
        std::vector<std::byte> bytes;
        assert(render_system.configuration.encode(&candidate, bytes));
        scene::SceneDescriptionBuilder input;
        assert(input.addSystem(
            {1},
            "render",
            render_system.type,
            render_system.description->version,
            render_system.description->configuration_schema_name,
            render_system.description->configuration_schema_version,
            bytes
        ));
        auto invalid_description = std::move(input).buildResolved();
        assert(invalid_description);
        const auto rejected =
            build_scene(std::make_shared<const scene::SceneDescription>(std::move(*invalid_description)));
        assert(!rejected);
        const auto* failure = std::get_if<scene::SceneBuildFailure>(&rejected.error().cause);
        assert(failure && failure->code == scene::ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE);
        assert(failure->scene_system.code == scene::ESceneSystemBuildError::INVALID_DESCRIPTION);
    }
    auto made_scene = build_scene(std::make_shared<const scene::SceneDescription>(std::move(*description)));
    assert(made_scene && scenes->pauseSimulation(made_scene->id()));
    const auto scene_id = made_scene->id();
    auto& registry = scenes->borrowInstance(scene_id)->get();
    const auto entity = registry.create();
    registry.emplace<sample_ext::Tint>(entity);
    const auto* state = scene::RenderSceneState::find(registry, {1});
    assert(state);
    until([&] { return resources->sceneReceipt(state->resource).status().state == scene::ESceneResourceState::READY; });
    const auto view_entity = registry.create();
    registry.emplace<scene::RenderViewRequest>(
        view_entity,
        system::SystemInstanceId{1},
        simulation::ecs::NullEntity,
        scene::ViewConfig{{64, 64}}
    );
    until([&] {
        const auto updated = scenes->driveFrame();
        assert(updated && updated->empty());
        const auto* result = registry.try_get<scene::RenderViewResult>(view_entity);
        assert(!result || !result->failure);
        return result && result->published_revision == 1;
    });
    const auto draw = [&] {
        for (unsigned index{}; index < 5; ++index)
        {

            const auto updated = scenes->driveFrame();
            assert(updated && updated->empty());
            render::TRenderProgram<> frame;
            render::RenderProgramBuilder<> commands(frame);
            commands.begin({});
            frame.kind = render::ERenderProgramKind::FRAME;
            const auto before = runtime->statistics().frames;
            until([&] {
                const auto submitted = runtime->submit(frame);
                assert(submitted);
                return *submitted == render::EFrameSubmit::SUBMITTED;
            });
            until([&] { return runtime->statistics().frames > before; });
        }
    };
    const auto pixels = [&] {
        std::vector<std::byte> bytes(64 * 64 * 8); // Default Scene output can be RGBA16F.
        const auto view = registry.get<scene::RenderViewResult>(view_entity).view;
        const auto output = resources->viewOutput(view);
        assert(output);
        const auto info = resources->outputInfo(*output);
        assert(info);
        auto request = control.readbackTarget(info->target, bytes.data(), bytes.size());
        until([&] { return request.isReady(); });
        assert(request.tryResult() && request.tryResult()->get().status == 0);
        return std::pair{std::move(bytes), request.tryResult()->get().format};
    };
    draw();
    const auto red_pixels = pixels();
    registry.patch<sample_ext::Tint>(entity, [](auto& tint) {
        tint.red = 0;
        tint.green = 1;
    });
    draw();
    const auto green_pixels = pixels();
    assert(red_pixels.second == green_pixels.second);
    // Offscreen presentation is BGRA8 sRGB. Inspect a covered pixel, including alpha.
    assert(unsigned(red_pixels.second) == 50);
    const auto channel = [](const auto& pixels, unsigned channel) {
        return std::to_integer<unsigned>(pixels.first[(32 * 64 + 32) * 4 + channel]);
    };
    assert(channel(red_pixels, 2) > 240 && channel(red_pixels, 1) < 8 && channel(red_pixels, 3) > 240);
    assert(channel(green_pixels, 1) > 240 && channel(green_pixels, 2) < 8 && channel(green_pixels, 3) > 240);
    assert(red_pixels.first != green_pixels.first); // Real ECS stage changed actual GPU output.
    assert(
        runtime->statistics().validation_errors == 0 && resources->sceneReceipt(*old_scene).status().scene == old_id
    );
    std::printf(
        "PASS external GPU triangle + ECS update, format=%u; late append, rollback, cancellation, stable op=%u\n",
        unsigned(red_pixels.second),
        stable_operation
    );
    assert(scenes->retireInstance(scene_id));
    scenes.reset();
    resources->release(*old_scene);
    until([&] { return resources->empty(); });
    auto during_close = first;
    during_close.factory.name = "ClosingCandidate";
    during_close.factory.descriptor.canonical_name = "sample.closing.candidate";
    during_close.factory.descriptor.type = render::featureId(during_close.factory.descriptor.canonical_name);
    assert(runtime->beginFeatureRegistration({during_close}));
    pump();
    assert(tasks.join());
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 8, programs = 4;
        const auto closed = runtime->advanceClose(replies, controls, programs);
        assert(closed);
        return *closed == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    assert(runtime->status().state == render::ERenderRuntimeState::RETIRED);
    assert(runtime->featureRegistrationStatus().state == render::EFeatureRegistrationState::CANCELLED);
    const auto stopped_registration = runtime->beginFeatureRegistration({during_close});
    assert(!stopped_registration && stopped_registration.error().code == render::ERendererError::STOPPING);
    // Drop every caller-owned export/configuration borrow. The backend still owns its
    // code until its shutdown owner is destroyed, even after all Feature instances retire.
    bindings = {};
    rendering->bindings.clear();
    *components = simulation::ecs::ComponentSchemaSet{};
    libraries.clear();
    library.reset();
    registration.code_lifetime.reset();
    first.code_lifetime.reset();
    failing.code_lifetime.reset();
    too_many.code_lifetime.reset();
    during_close.code_lifetime.reset();
    assert(!module_code.expired());
    resources.reset();
    runtime.reset();
    assert(module_code.expired());
}
