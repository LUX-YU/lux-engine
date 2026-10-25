#include "GraphTriangle.hpp"
#include "RenderTest.hpp"
#include "SurfaceSample.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/FeatureRegistration.hpp>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/rendering/WindowSurface.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;
using namespace integration_test;

namespace
{
    struct ClearConfiguration final
    {
        TargetDesc target;
        bool fail_preparation = false;
        bool sample_input = false;
        bool operator==(const ClearConfiguration&) const noexcept = default;
    };

    struct RejectedPreparation final
    {
        detail::DeviceSession& device;
        nri::Buffer* buffer;
        detail::ResourceUpdate output;

        ~RejectedPreparation() noexcept
        {
            device.deferRelease([core = &device.nriCore(), buffer = buffer]() noexcept { core->DestroyBuffer(buffer); }
            );
        }
    };

    struct ClearFeature final
    {
        using Config = ClearConfiguration;
        static constexpr FeatureName name{"test.presentation.clear"};

        static RenderResult<void> describe(const void* input, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& target = static_cast<const Config*>(input)->target;
            GraphImageDesc color{target.width, target.height, 1, 1, target.format};
            color.extent.source = EImageExtentSource::TARGET;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
            if (static_cast<const Config*>(input)->sample_input)
            {
                GraphImageDesc source{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
                source.extent.source = EImageExtentSource::IMPORTED;
                ports.resources.push_back({"source", EFeaturePort::INPUT, source});
            }
            if (static_cast<const Config*>(input)->fail_preparation)
            {
                ports.resources.push_back(
                    {"preparation",
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{4},
                     EGraphOrigin::IMPORTED,
                     {GraphBufferRange{0, 4}}}
                );
            }
            return {};
        }

        using Storages = std::tuple<>;
        using Commands = std::tuple<>;

        static const FeatureGraphOps& graph(bool) noexcept
        {
            static const FeatureGraphOps ops{
                describe,
                [](const void* value, std::string_view, FeatureBuildContext& context) noexcept -> RenderResult<void>
                {
                    if (static_cast<const ClearConfiguration*>(value)->fail_preparation)
                    {
                        detail::FeatureFrameContribution preparation;
                        const auto resource = context.resource("preparation").resource;
                        preparation.outputs = {resource};
                        preparation.configuration = take(ConfigValue::make(resource));
                        preparation.create = [](detail::DeviceSession& device,
                                                detail::FrameExecution&,
                                                ResourceRegistry&,
                                                std::span<const StorageId>,
                                                const void* value) noexcept -> RenderResult<detail::FeatureBinding>
                        {
                            nri::Buffer* buffer = nullptr;
                            const nri::BufferDesc description{4, 0, nri::BufferUsageBits::NONE};
                            check(
                                device.nriCore()
                                    .CreatePlacedBuffer(device.device(), NriDeviceHeap, description, buffer) ==
                                nri::Result::SUCCESS
                            );
                            auto* state = new RejectedPreparation{
                                device,
                                buffer,
                                {*static_cast<const GraphResourceId*>(value), {buffer, 0, 4}}
                            };
                            detail::FeatureBinding binding;
                            binding.value = {
                                state,
                                [](void* object) noexcept { delete static_cast<RejectedPreparation*>(object); }
                            };
                            binding.initial_resources = {&state->output, 1};
                            binding.prepare =
                                [](void*, const detail::FeatureFrameInput&, detail::ResourceUploader&) noexcept
                                -> RenderResult<detail::FeatureBinding::Preparation>
                            { return cxx::unexpected(RenderError{kDeviceArgument}); };
                            return binding;
                        };
                        context.bindings.push_back(std::move(preparation));
                    }
                    if (static_cast<const ClearConfiguration*>(value)->sample_input)
                    {
                        rdesc::SpirvArtifact shader;
                        shader.stages.push_back(
                            {rdesc::EShaderType::VERTEX, "main", {std::begin(kGraphTriangle), std::end(kGraphTriangle)}}
                        );
                        shader.stages.push_back(
                            {rdesc::EShaderType::FRAGMENT,
                             "main",
                             {std::begin(kSurfaceSample), std::end(kSurfaceSample)}}
                        );
                        check(rdesc::ShaderInfo::deserialize(kSurfaceSampleReflection, shader.interface));
                        shader.interface.entry_points.push_back({"main", rdesc::EShaderType::VERTEX});
                        RasterWork work;
                        work.interface.program = take(context.graph.addProgram(std::move(shader)));
                        work.interface.resources = {{"source_image", {context.resource("source")}}};
                        work.interface.samplers = {{"source_sampler", {context.addSampler({})}}};
                        work.colors.push_back({{context.resource("color")}});
                        auto pass = context.graph.addRasterPass("sample", std::move(work));
                        if (!pass)
                        {
                            return cxx::unexpected(pass.error());
                        }
                        return context.publish("color", pass->written.front());
                    }
                    auto clear = context.graph.addTransferPass(
                        "clear",
                        TransferWork{{ClearImage{{context.resource("color")}, {0.1f, 0.3f, 0.6f, 1}}}}
                    );
                    if (!clear)
                    {
                        return cxx::unexpected(clear.error());
                    }
                    return context.publish("color", clear->written.front());
                }
            };
            return ops;
        }
    };

} // namespace

int main(int argc, char**)
{
    poll_events = &window::LuxWindow::pollEvents;
    auto platform = take(window::GlfwRuntime::create());
    auto first = take(window::LuxWindow::create({320, 240, "NRI presentation 1"}));
    auto second = take(window::LuxWindow::create({400, 240, "NRI presentation 2"}));
    auto execution = take(execution::ExecutionRuntime::create({2, 128, {16}}));
    RenderExecutionConfig config;
    config.render.channel_name = "engine.presentation";
    config.render.validation = true;
    if (argc > 1)
    {
        config.render.uploads.emplace();
    }
    config.render.frames_in_flight = 3;
    FeatureId feature;
    config.feature_registration = &feature;
    config.register_features = [](void* destination, RenderServer& server) noexcept -> RenderResult<void>
    {
        auto feature = server.registerFeature<ClearFeature>();
        if (!feature)
        {
            return cxx::unexpected(feature.error());
        }
        *static_cast<FeatureId*>(destination) = *feature;
        return {};
    };
    auto rendering = take(RenderExecution::create(std::move(config), execution));
    const auto scene = call<CreateScene>(*rendering, {}).scene;
    std::vector<TargetCreated> targets;
    std::vector<EffectId> effects;
    std::vector<AttachmentId> writers;
    std::vector<ViewId> views;
    for (auto* window : {first.get(), second.get()})
    {
        TargetDesc description{320, 240, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE};
        window->framebufferSize(description.width, description.height);
        SurfaceDescription surface;
        check(makeWindowSurface(window->nativeWindow(), false, surface));
        auto target = call<CreateTarget>(*rendering, {description, surface});
        targets.push_back(target);
        GraphEffectRequest effect{target.description};
        effect.definition.features.push_back(
            {feature, "clear", take(ConfigValue::make(ClearConfiguration{target.description}))}
        );
        effect.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.description.format, 1}});
        auto compiled = call<CreateGraphEffect>(*rendering, std::move(effect)).effect;
        effects.push_back(compiled);
        const auto view = call<CreateView>(*rendering, {scene}).view;
        views.push_back(view);
        writers.push_back(call<Attach>(*rendering, {scene, view, target.target, compiled}).attachment);
    }
    for (unsigned i = 0; i < 24; ++i)
    {
        frame(*rendering);
    }
    check(rendering->stats().presentations >= 48);
    // The producer is created after both surfaces. Correct order follows the declared image
    // relationship, never TargetId order. UI and Scene use the same physical FIF slot.
    TargetDesc image_description{128, 96, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    auto image = call<CreateTarget>(*rendering, {image_description});
    GraphEffectRequest producer{image_description};
    producer.definition.features.push_back(
        {feature, "producer", take(ConfigValue::make(ClearConfiguration{image_description}))}
    );
    producer.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{image_description.format, 1}});
    const auto producer_program = call<CreateGraphEffect>(*rendering, std::move(producer)).effect;
    effects.push_back(producer_program);
    const auto producer_view = call<CreateView>(*rendering, {scene}).view;
    const auto producer_attachment =
        call<Attach>(*rendering, {scene, producer_view, image.target, producer_program}).attachment;
    const auto make_consumer = [&](TargetDesc target)
    {
        GraphEffectRequest consumer{target};
        consumer.definition.features.push_back(
            {feature, "consumer", take(ConfigValue::make(ClearConfiguration{target, false, true}))}
        );
        consumer.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.format, 1}});
        GraphImageDesc source{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
        source.extent.source = EImageExtentSource::IMPORTED;
        consumer.definition.inputs.push_back(
            {{0, "source"}, {"scene-output", source, EGraphOrigin::IMPORTED, {GraphImageRange{}}, EGraphUse::SAMPLED}}
        );
        auto program = call<CreateGraphEffect>(*rendering, std::move(consumer)).effect;
        effects.push_back(program);
        return program;
    };
    const auto consumer_program = make_consumer(targets[1].description);
    AttachmentRequest consumer{scene, views[1], targets[1].target, consumer_program};
    consumer.replaces = writers[1];
    consumer.target_images.push_back({"scene-output", image.target});
    writers[1] = call<Attach>(*rendering, std::move(consumer)).attachment;
    for (unsigned i = 0; i < 6; ++i)
    {
        frame(*rendering);
    }
    image_description.width = 256;
    image_description.height = 144;
    const auto image_resize = call<ResizeTarget>(*rendering, {image.target, image_description});
    // A queued activation from the old epoch must not resume new output with stale parameters.
    expected_frame_error = kRuntimeState;
    call<SetTargetActive>(*rendering, {image.target, true, image.epoch}, kRuntimeState);
    // A cycle must not replace the valid producer or any of its descriptor banks.
    const auto cycle_program = make_consumer(image_description);
    AttachmentRequest cycle{scene, producer_view, image.target, cycle_program};
    cycle.replaces = producer_attachment;
    cycle.target_images.push_back({"scene-output", image.target});
    expected_frame_error = kCommandContract;
    call<Attach>(*rendering, std::move(cycle), kCommandContract);
    expected_frame_error = kRuntimeState;
    call<RemoveTarget>(*rendering, image.target, kRuntimeState);
    call<SetTargetActive>(*rendering, {image.target, false});
    const auto missing_producer = rendering->stats().presentations;
    frame(*rendering);
    check(rendering->stats().presentations == missing_producer + 1);
    call<SetTargetActive>(*rendering, {image.target, true, image_resize.epoch});

    call<SetTargetActive>(*rendering, {targets[0].target, false});
    const auto before = rendering->stats().presentations;
    for (unsigned i = 0; i < 8; ++i)
    {
        frame(*rendering);
    }
    check(rendering->stats().presentations == before + 8);
    call<SetTargetActive>(*rendering, {targets[0].target, true});
    // Same-window rebuild exercises WSI retirement independently from ordinary GPU frame completion.
    const auto surface_resize = call<ResizeTarget>(*rendering, {targets[0].target, targets[0].description});
    call<SetTargetActive>(*rendering, {targets[0].target, true, surface_resize.epoch});
    for (unsigned i = 0; i < 8; ++i)
    {
        frame(*rendering);
    }
    for (const auto& target : targets)
    {
        call<RemoveTarget>(*rendering, target.target);
    }
    call<RemoveTarget>(*rendering, image.target);
    // Acquire succeeded, but preparation rejects before any rendering submit. Removing that
    // Target must still consume its WSI acquire signal before returning the window borrow.
    SurfaceDescription abandoned_surface;
    check(makeWindowSurface(first->nativeWindow(), false, abandoned_surface));
    auto abandoned = call<CreateTarget>(*rendering, {targets[0].description, abandoned_surface});
    GraphEffectRequest failed_effect{abandoned.description};
    failed_effect.definition.features.push_back(
        {feature, "clear", take(ConfigValue::make(ClearConfiguration{abandoned.description, true}))}
    );
    failed_effect.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{abandoned.description.format, 1}}
    );
    failed_effect.definition.outputs.push_back({{0, "preparation"}});
    const auto failed_program = call<CreateGraphEffect>(*rendering, std::move(failed_effect)).effect;
    const auto abandoned_view = call<CreateView>(*rendering, {scene}).view;
    expected_frame_error = kDeviceArgument;
    call<Attach>(*rendering, {scene, abandoned_view, abandoned.target, failed_program});
    check(resynchronize);
    call<RemoveTarget>(*rendering, abandoned.target);
    effects.push_back(failed_program);
    first.reset();
    second.reset(); // Actual RemoveTarget responses returned both native-window borrows.
    call<RemoveScene>(*rendering, {scene});
    for (auto effect : effects)
    {
        call<RemoveEffect>(*rendering, {effect});
    }
    check(bool(rendering->shutdown()));
    const auto stats = rendering->stats();
    check(stats.validation_errors == 0);
    rendering.reset();
    execution.requestStop();
    check(bool(execution.join()));
    std::printf(
        "Presentation: %llu accepted presents, validation errors=%llu warnings=%llu\n",
        static_cast<unsigned long long>(stats.presentations),
        static_cast<unsigned long long>(stats.validation_errors),
        static_cast<unsigned long long>(stats.validation_warnings)
    );
}
