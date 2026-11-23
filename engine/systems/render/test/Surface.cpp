#include "GraphTriangle.hpp"
#include "SceneCapture.hpp"
#include "SurfaceSample.hpp"
#include <lux/engine/Init.hpp>
#include <lux/engine/Engine.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/commands/Registration.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/core/execution/CompletionWork.hpp>
#include <limits>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/render/frame/FrameStorage.hpp>
#include <lux/engine/systems/render/WindowSurface.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace lux;
using namespace lux::render;
using namespace lux::engine::render;

namespace
{
    template <class Result> auto take(Result result)
    {
        CHECK(bool(result));
        return std::move(*result);
    }

    template <RenderCommand Command>
    typename Command::Response
    call(RenderClient& runtime, typename Command::Request input, error::ErrorId expected_error = {})
    {
        auto& execution = execution::ExecutionRuntime::instance();
        std::optional<typename Command::Response> response;
        CHECK(execution.spawn(stdexec::then(
            stdexec::starts_on(execution.mainScheduler(), runtime.request<Command>(input)),
            [&](typename Command::Response value) noexcept { response.emplace(std::move(value)); }
        )));
        CHECK(execution.waitUntil(
            [&]() noexcept
            {
                window::LuxWindow::pollEvents();
                return response.has_value();
            }
        ));
        if (response->error.type != expected_error)
        {
            std::fprintf(
                stderr, "Surface command %.*s error=%llu expected=%llu\n",
                int(Command::name.size()), Command::name.data(), response->error.type, expected_error
            );
        }
        CHECK(response->error.type == expected_error);
        return std::move(*response);
    }

    // Controlled preparation failure, observed only on Render's thread. No production failure switch.
    std::atomic<error::ErrorId> preparation_error{kDeviceArgument};
    std::atomic_uint reject_submission{};
    std::atomic_bool hold_preparation{false};
    nri::CoreInterface* submission_operations = nullptr;
    decltype(nri::CoreInterface::QueueSubmit) submit_native = nullptr;
    unsigned submissions_before_failure = 0;

    nri::Result NRI_CALL rejectOneSubmission(nri::Queue& queue, const nri::QueueSubmitDesc& input)
    {
        if (--submissions_before_failure == 0)
        {
            submission_operations->QueueSubmit = submit_native;
            return nri::Result::FAILURE; // The rejected call never reaches Vulkan; previous calls really did.
        }
        return submit_native(queue, input);
    }

    RenderFrameSubmission::Result waitFrameOperation(exec::task<RenderFrameSubmission::Result> operation)
    {
        auto& execution = execution::ExecutionRuntime::instance();
        std::optional<RenderFrameSubmission::Result> result;
        CHECK(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(operation))),
            [&](auto value) noexcept
            {
                if (value) result.emplace();
                else
                {
                    const auto* failure = value.error().domainFailure();
                    CHECK(failure != nullptr);
                    result.emplace(cxx::unexpected(*failure));
                }
            }
        )));
        CHECK(execution.waitUntil([&]() noexcept
        {
            window::LuxWindow::pollEvents();
            return result.has_value();
        }));
        return std::move(*result);
    }

    FrameCompletion frame(error::ErrorId expected_error = {})
    {
        auto& frames = RenderFrameSubmission::instance();
        CHECK(bool(waitFrameOperation(frames.beginFrame())));
        std::optional<FrameCompletion> completion;
        frames.writer().onCompletion(&completion, [](void* state, const FrameCompletion& value) noexcept
        {
            static_cast<std::optional<FrameCompletion>*>(state)->emplace(value);
        });
        CHECK(bool(frames.submitFrame()));
        CHECK(bool(waitFrameOperation(frames.whenSubmittedFramesConsumed())));
        CHECK(completion.has_value());
        if (completion->error.type != expected_error)
        {
            std::fprintf(stderr, "Frame %llu outcome=%u batches=%u error=%llu expected=%llu\n",
                completion->sequence, unsigned(completion->outcome), completion->submitted_batches,
                completion->error.type, expected_error);
        }
        CHECK(completion->error.type == expected_error);
        if (!expected_error) CHECK(completion->outcome == EFrameOutcome::SUBMITTED);
        return *completion;
    }

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

        static const FeatureGraphOps& graph(bool) noexcept
        {
            static const FeatureGraphOps ops{
                describe,
                [](
                    const void* value, std::string_view, FeatureBuildContext& context,
                    const lux::render::detail::EnabledDeviceFeatures&
                ) noexcept -> RenderResult<void>
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
                                                detail::RenderGraphInstance&,
                                                detail::CompiledPrograms&,
                                                std::span<const StorageId>,
                                                const void* value) noexcept -> RenderResult<detail::FeatureBinding>
                        {
                            nri::Buffer* buffer = nullptr;
                            const nri::BufferDesc description{4, 0, nri::BufferUsageBits::NONE};
                            CHECK(
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
                            binding.preparation = detail::FeatureBinding::PrepareResources{
                                [](void* value, const detail::FeatureFrameInput&, detail::ResourceUploader&) noexcept
                                    -> RenderResult<detail::FeatureBinding::Preparation>
                                {
                                    if (const auto countdown = reject_submission.exchange(0))
                                    {
                                        auto& state = *static_cast<RejectedPreparation*>(value);
                                        submission_operations =
                                            const_cast<nri::CoreInterface*>(&state.device.nriCore());
                                        submit_native = submission_operations->QueueSubmit;
                                        submissions_before_failure = countdown;
                                        submission_operations->QueueSubmit = rejectOneSubmission;
                                    }
                                    hold_preparation.wait(true);
                                    const auto failure = preparation_error.load();
                                    if (failure) return cxx::unexpected(RenderError{failure});
                                    return detail::FeatureBinding::Preparation{};
                                }
                            };
                            return binding;
                        };
                        context.bindings.push_back(std::move(preparation));
                    }
                    if (static_cast<const ClearConfiguration*>(value)->sample_input)
                    {
                        auto shader = kSurfaceSample.load(rdesc::EShaderTarget::SPIRV);
                        auto vertex = kGraphTriangle.load(rdesc::EShaderTarget::SPIRV);
                        shader.stages.insert(shader.stages.begin(), std::move(vertex.stages.front()));
                        shader.interface.entry_points.insert(
                            shader.interface.entry_points.begin(),
                            vertex.interface.entry_points.begin(),
                            vertex.interface.entry_points.end()
                        );
                        RasterWork work;
                        work.fixed().interface.program = take(context.graph.addProgram(std::move(shader)));
                        work.fixed().interface.resources = {{"source_image", {context.resource("source")}}};
                        work.fixed().interface.samplers = {{"source_sampler", {context.addSampler({})}}};
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
                    if (static_cast<const ClearConfiguration*>(value)->fail_preparation)
                    {
                        context.bindings.back().passes.push_back(clear->pass);
                    }
                    return context.publish("color", clear->written.front());
                }
            };
            return ops;
        }
    };

    RenderResult<CapabilityGraphContribution> lowerSurfaceProbe(const CapabilityBuildInput& input) noexcept
    {
        const auto& config = *static_cast<const ClearConfiguration*>(input.configuration.data());
        CapabilityGraphContribution result;
        result.definition.features.push_back({input.features[0], "surface", take(ConfigValue::make(config))});
        if (config.sample_input)
        {
            GraphImageDesc source{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
            source.extent.source = EImageExtentSource::IMPORTED;
            result.definition.inputs.push_back(
                {{0, "source"},
                 {"scene-output", source, EGraphOrigin::IMPORTED, {GraphImageRange{}}, EGraphUse::SAMPLED}}
            );
        }
        result.ports.push_back({{0, "color"}});
        return result;
    }

    // Run under the external Acquire NOT_READY injector. The first window keeps a real
    // acquired image while the second remains unavailable until the public stop request.
    void surfaceBoundaryFailure(std::string_view scenario)
    {
        const bool wait_stop = scenario == "wait-stop";
        auto execution = take(execution::ExecutionRuntime::create({2, 128, {16}}));
        std::unique_ptr<RenderClient> runtime;
        execution::CompletionWork responses(*execution, &runtime, [](void* state) noexcept
        {
            auto& endpoint = *static_cast<std::unique_ptr<RenderClient>*>(state)->get();
            CHECK(bool(endpoint.dispatchResponses(std::numeric_limits<std::size_t>::max())));
        });
        RenderServerConfig config{"test.presentation.wait-stop", true};
        config.frames_in_flight = 2;
        config.max_frames_ahead = 2;
        config.uploads.emplace();
        std::array<std::atomic_uint, 2> errors{};
        config.diagnostics = {&errors, [](void* state, const RenderDiagnostic& diagnostic) noexcept
        {
            if (diagnostic.severity != ERenderDiagnosticSeverity::ERROR_MESSAGE) return;
            const bool injected_present = diagnostic.file.ends_with("SwapChainVK.hpp") &&
                diagnostic.message.find("QueuePresentKHR(): failed, result") != std::string_view::npos;
            ++(*static_cast<std::array<std::atomic_uint, 2>*>(state))[injected_present ? 0 : 1];
            std::fprintf(stderr, "%.*s\n", int(diagnostic.message.size()), diagnostic.message.data());
        }};
        config.response_context = &responses;
        config.responses_available = [](void* state) noexcept
        { static_cast<execution::CompletionWork*>(state)->request(); };
        auto server = take(RenderServer::create(config, *execution));
        auto feature = take(server->registerFeature<ClearFeature>());
        const auto implementation = registerRenderProbe<ClearConfiguration>(
            *server, "test.wait-stop", {feature}, probePorts({"color"}, {}), {}, lowerSurfaceProbe
        );
        auto client = take(platform::ipc::IpcCore::instance().createRequester(config.channel_name));
        CHECK(server->setFrameSource(client.id()));
        RenderResult<void> stopped;
        std::thread render_thread([&]() noexcept { stopped = server->run(); });
        runtime = take(RenderClient::create(std::move(client)));
        auto first = take(window::LuxWindow::create({320, 240, "Wait Stop 1"}));
        auto second = take(window::LuxWindow::create({320, 240, "Wait Stop 2"}));
        const auto scene = call<CreateScene>(*runtime, {}).scene;
        for (auto* window : {first.get(), second.get()})
        {
            TargetDesc description{320, 240, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE};
            SurfaceDescription surface;
            CHECK(makeWindowSurface(window->nativeWindow(), false, surface));
            const auto target = call<CreateTarget>(*runtime, {description, surface});
            auto request = probeRequest(
                target.description, implementation, ClearConfiguration{target.description}, {"color"}
            );
            const auto effect = call<CreateEffect>(*runtime, std::move(request)).effect;
            const auto view = call<CreateView>(*runtime, {scene}).view;
            call<Attach>(*runtime, {scene, view, target.target, effect});
        }
        std::array<RenderFrame, 3> packets{RenderFrame{1}, RenderFrame{2}, RenderFrame{3}};
        std::array<std::optional<FrameCompletion>, 3> completions;
        const auto frame_count = wait_stop ? 3u : 1u;
        const auto submitted = std::span(completions).first(frame_count);
        for (std::size_t i = 0; i < frame_count; ++i)
        {
            CHECK(execution->spawn(stdexec::then(runtime->request<SubmitFrame>(packets[i]), [&, i](auto result) noexcept
            { completions[i].emplace(result); })));
        }
        auto& completion = completions.front();
        if (wait_stop)
        {
            bool elapsed = false;
            auto delay = execution::asResult(stdexec::continues_on(
                execution->timer().after(std::chrono::milliseconds(250)), execution->mainScheduler()
            ));
            CHECK(execution->spawn(stdexec::then(std::move(delay), [&](auto result) noexcept
            {
                CHECK(result);
                elapsed = true;
            })));
            CHECK(execution->waitUntil([&]() noexcept
            {
                window::LuxWindow::pollEvents();
                return elapsed;
            }));
            CHECK(std::ranges::none_of(submitted, [](const auto& value) { return value.has_value(); }));
        }
        else
        {
            CHECK(execution->waitUntil([&]() noexcept
            {
                window::LuxWindow::pollEvents();
                return completion.has_value();
            }));
        }
        const auto begin = std::chrono::steady_clock::now();
        server->stop();
        CHECK(execution->waitUntil([&]() noexcept
        {
            window::LuxWindow::pollEvents();
            return std::ranges::all_of(submitted, [](const auto& value) { return value.has_value(); });
        }));
        render_thread.join();
        const auto milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin
        ).count();
        CHECK(milliseconds < 3000);
        if (wait_stop)
        {
            CHECK(stopped);
            for (const auto& value : submitted)
            {
                CHECK(value->error.type == kCommandCancelled);
                CHECK(value->outcome == EFrameOutcome::NOT_SUBMITTED && value->submitted_batches == 0);
            }
        }
        else
        {
            const auto native = scenario == "present-out-of-date" ? nri::Result::OUT_OF_DATE :
                scenario == "present-oom" ? nri::Result::OUT_OF_MEMORY : nri::Result::FAILURE;
            CHECK(!stopped && stopped.error().type == kDeviceFailure);
            CHECK(completion->error.type == kDeviceFailure);
            CHECK(completion->error.args[0] == static_cast<std::uint32_t>(native));
            CHECK(completion->outcome == EFrameOutcome::PARTIALLY_SUBMITTED && completion->submitted_batches > 0);
            CHECK(errors[0] == 1); // Exactly the deliberately injected native error; no blanket suppression.
        }
        CHECK(errors[1] == 0);
        responses.cancel();
        runtime->serverStopped(stopped ? RenderError{} : stopped.error());
        runtime.reset();
        CHECK(server->stats().validation_errors == errors[0]);
        server.reset(); // Returns the native window borrows before either window is destroyed.
        std::printf("BOUNDARY PASS scenario=%.*s frames=%u outcome=%u stop_to_join_ms=%.3f unexpected_errors=0\n",
            int(scenario.size()), scenario.data(), frame_count, unsigned(completion->outcome), milliseconds);
    }

    // OS actions are issued by the external controller against this process's own window.
    // No platform command or synthetic minimized flag enters the production window module.
    void minimizeAndRestore(window::LuxWindow& window, RenderServer& server)
    {
        std::puts("MINIMIZE_READY");
        std::fflush(stdout);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!window.minimized() && std::chrono::steady_clock::now() < deadline)
        {
            window::LuxWindow::waitEvents(0.01);
        }
        CHECK(window.minimized());
        std::puts("MINIMIZED_OBSERVED");
        std::fflush(stdout);
        const auto before = server.stats().presentations;
        for (unsigned i = 0; i < 8; ++i) frame();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (window.minimized() && std::chrono::steady_clock::now() < deadline)
        {
            window::LuxWindow::waitEvents(0.01);
        }
        CHECK(!window.minimized());
        for (unsigned i = 0; i < 8; ++i) frame();
        CHECK(server.stats().presentations == before + 32);
        std::puts("RESTORE_PASS new_presents=32");
        std::fflush(stdout);
    }

} // namespace

int main(int argc, char** argv)
{
    auto platform = take(window::GlfwRuntime::create());
    const std::string_view scenario = argc > 1 ? argv[1] : "";
    if (scenario == "wait-stop" || scenario.starts_with("present-") || scenario == "retire-timeout")
    {
        surfaceBoundaryFailure(scenario);
        return 0;
    }
    const bool minimize = argc > 1 && std::string_view(argv[1]) == "minimize";
    CapabilityImplementationId implementation;
    RenderServer* server = nullptr;
    std::atomic_uint validation_errors{};
    RenderServerConfig config{"engine.presentation", true};
    config.frames_in_flight = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 3;
    if (minimize || (argc > 1 && std::string_view{argv[1]} == "uploads")) config.uploads.emplace();
    config.diagnostics = {&validation_errors, [](void* state, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
        {
            ++*static_cast<std::atomic_uint*>(state);
            std::fprintf(stderr, "%.*s\n", int(diagnostic.message.size()), diagnostic.message.data());
        }
    }};
    execution::ExecutionRuntimeConfig execution_config{2, 128, {16}, execution::BlockingSchedulerConfig{2, 16}};
    DeviceOptions device;
    device.texture_descriptors = true; // The offscreen producer is sampled by the second surface.
    auto context = take(engine::init(
        execution_config,
        asset::AssetDecodeLimits{67108864, 67108864, 64},
        config,
        device,
        {},
        {},
        [&](RenderServer& service) noexcept -> RenderResult<void>
        {
            server = &service;
            auto feature = service.registerFeature<ClearFeature>();
            if (!feature) return cxx::unexpected(feature.error());
            implementation = registerRenderProbe<ClearConfiguration>(
                service, "test.presentation", {*feature}, probePorts({"color"}, {}), {}, lowerSurfaceProbe
            );
            return {};
        }
    ));
    auto* rendering = &context->renderClient();
    auto first = take(window::LuxWindow::create({320, 240, "NRI presentation 1"}));
    auto second = take(window::LuxWindow::create({400, 240, "NRI presentation 2"}));
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
        CHECK(makeWindowSurface(window->nativeWindow(), false, surface));
        auto target = call<CreateTarget>(*rendering, {description, surface});
        targets.push_back(target);
        auto effect = probeRequest(
            target.description, implementation, ClearConfiguration{target.description}, {"color"}
        );
        auto compiled = call<CreateEffect>(*rendering, std::move(effect)).effect;
        effects.push_back(compiled);
        const auto view = call<CreateView>(*rendering, {scene}).view;
        views.push_back(view);
        writers.push_back(call<Attach>(*rendering, {scene, view, target.target, compiled}).attachment);
    }
    for (unsigned i = 0; i < 24; ++i)
    {
        frame();
    }
    CHECK(server->stats().presentations >= 48);
    if (minimize) minimizeAndRestore(*first, *server);
    // The producer is created after both surfaces. Correct order follows the declared image
    // relationship, never TargetId order. UI and Scene use the same physical FIF slot.
    TargetDesc image_description{128, 96, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    auto image = call<CreateTarget>(*rendering, {image_description});
    auto producer = probeRequest(image_description, implementation, ClearConfiguration{image_description}, {"color"});
    const auto producer_program = call<CreateEffect>(*rendering, std::move(producer)).effect;
    effects.push_back(producer_program);
    const auto producer_view = call<CreateView>(*rendering, {scene}).view;
    const auto producer_attachment =
        call<Attach>(*rendering, {scene, producer_view, image.target, producer_program}).attachment;
    const auto make_consumer = [&](TargetDesc target)
    {
        auto consumer = probeRequest(target, implementation, ClearConfiguration{target, false, true}, {"color"});
        auto program = call<CreateEffect>(*rendering, std::move(consumer)).effect;
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
        frame();
    }
    image_description.width = 256;
    image_description.height = 144;
    const auto image_resize = call<ResizeTarget>(*rendering, {image.target, image_description});
    const auto image_unchanged = call<ResizeTarget>(*rendering, {image.target, image_resize.description});
    CHECK(image_unchanged.epoch == image_resize.epoch && image_unchanged.description == image_resize.description);
    // A queued activation from the old epoch must not resume new output with stale parameters.
    call<SetTargetActive>(*rendering, {image.target, true, image.epoch}, kRuntimeState);
    // A cycle must not replace the valid producer or any of its descriptor banks.
    const auto cycle_program = make_consumer(image_description);
    AttachmentRequest cycle{scene, producer_view, image.target, cycle_program};
    cycle.replaces = producer_attachment;
    cycle.target_images.push_back({"scene-output", image.target});
    call<Attach>(*rendering, std::move(cycle), kCommandContract);
    call<RemoveTarget>(*rendering, image.target, kRuntimeState);
    call<SetTargetActive>(*rendering, {image.target, false});
    const auto missing_producer = server->stats().presentations;
    frame();
    CHECK(server->stats().presentations == missing_producer + 1);
    call<SetTargetActive>(*rendering, {image.target, true, image_resize.epoch});

    call<SetTargetActive>(*rendering, {targets[0].target, false});
    const auto before = server->stats().presentations;
    for (unsigned i = 0; i < 8; ++i)
    {
        frame();
    }
    CHECK(server->stats().presentations == before + 8);
    call<SetTargetActive>(*rendering, {targets[0].target, true});
    // Restoring an unchanged healthy output must preserve its epoch and presentation objects.
    const auto surface_unchanged = call<ResizeTarget>(*rendering, {targets[0].target, targets[0].description});
    CHECK(surface_unchanged.epoch == targets[0].epoch);
    // Keep real WSI retirement coverage: change the window, then rebuild its actual output.
    auto placement = take(first->state()).placement;
    placement.normal.width += 32;
    placement.normal.height += 32;
    CHECK(first->applyPlacement(placement));
    window::LuxWindow::pollEvents();
    auto resized_description = targets[0].description;
    first->framebufferSize(resized_description.width, resized_description.height);
    CHECK(resized_description != targets[0].description);
    const auto surface_resize = call<ResizeTarget>(*rendering, {targets[0].target, resized_description});
    CHECK(surface_resize.epoch == targets[0].epoch + 1);
    targets[0] = surface_resize;
    call<SetTargetActive>(*rendering, {targets[0].target, true, surface_resize.epoch});
    for (unsigned i = 0; i < 8; ++i)
    {
        frame();
    }
    for (const auto& target : targets)
    {
        call<RemoveTarget>(*rendering, target.target);
    }
    call<RemoveTarget>(*rendering, image.target);
    // Acquire succeeded, but preparation rejects before any rendering submit. Removing that
    // Target must still consume its WSI acquire signal before returning the window borrow.
    SurfaceDescription abandoned_surface;
    CHECK(makeWindowSurface(first->nativeWindow(), false, abandoned_surface));
    auto abandoned = call<CreateTarget>(*rendering, {targets[0].description, abandoned_surface});
    auto failed_effect = probeRequest(
        abandoned.description, implementation, ClearConfiguration{abandoned.description, true}, {"color"}
    );
    const auto failed_program = call<CreateEffect>(*rendering, std::move(failed_effect)).effect;
    const auto abandoned_view = call<CreateView>(*rendering, {scene}).view;
    call<Attach>(*rendering, {scene, abandoned_view, abandoned.target, failed_program});
    const auto rejected = frame(kDeviceArgument);
    CHECK(rejected.requires_resynchronization && rejected.outcome == EFrameOutcome::NOT_SUBMITTED);
    CHECK(rejected.submitted_batches == 0);
    auto& frames = RenderFrameSubmission::instance();
    const auto hard_failure = waitFrameOperation(frames.beginFrame());
    CHECK(!hard_failure && hard_failure.error().type == kDeviceArgument);
    effects.push_back(failed_program);
    if (config.uploads)
    {
        // Keep the acquired image through multiple physical slot changes. Busy must terminate each
        // packet, admit structural commands, and invalidate collectors without stopping the driver.
        preparation_error = 0;
        frame(); // Complete resynchronization before queuing two packets from the same clean epoch.
        preparation_error = kDeviceBusy;
        hold_preparation = true;
        std::vector<FrameCompletion> queued_outcomes;
        for (unsigned i = 0; i < 2; ++i)
        {
            CHECK(waitFrameOperation(frames.beginFrame()));
            frames.writer().onCompletion(
                &queued_outcomes,
                [](void* state, const FrameCompletion& value) noexcept
                {
                    static_cast<std::vector<FrameCompletion>*>(state)->push_back(value);
                }
            );
            CHECK(frames.submitFrame());
        }
        hold_preparation = false;
        hold_preparation.notify_one();
        CHECK(waitFrameOperation(frames.whenSubmittedFramesConsumed()));
        CHECK(queued_outcomes.size() == 2);
        std::ranges::sort(queued_outcomes, {}, &FrameCompletion::sequence);
        CHECK(queued_outcomes[0].error.type == kDeviceBusy);
        CHECK(queued_outcomes[0].outcome == EFrameOutcome::NOT_SUBMITTED);
        CHECK(queued_outcomes[1].error.type == kFrameResynchronization);
        CHECK(queued_outcomes[1].outcome == EFrameOutcome::REJECTED);
        CHECK(queued_outcomes[1].requires_resynchronization);
        for (unsigned i = 0; i < config.frames_in_flight + 1; ++i)
        {
            const auto busy = frame(kDeviceBusy);
            CHECK(busy.outcome == EFrameOutcome::NOT_SUBMITTED && busy.submitted_batches == 0);
            CHECK(busy.requires_resynchronization);
            call<SetTargetActive>(*rendering, {abandoned.target, true});
        }
        preparation_error = 0;
        for (unsigned i = 0; i < config.frames_in_flight * 3; ++i) frame();
        const auto recovered_resize = call<ResizeTarget>(*rendering, {abandoned.target, abandoned.description});
        call<SetTargetActive>(*rendering, {abandoned.target, true, recovered_resize.epoch});
        for (unsigned i = 0; i < config.frames_in_flight * 2; ++i) frame();
        // Finally abandon another real acquire and remove it without replaying a failed frame.
        preparation_error = kDeviceBusy;
        CHECK(frame(kDeviceBusy).submitted_batches == 0);
        // Removing an acquired-but-unsubmitted target consumes its binary signal. Recreate it for
        // the partial-submit case, where a real accepted prefix must survive the CPU frame response.
        call<RemoveTarget>(*rendering, abandoned.target);
        preparation_error = 0;
        auto partial_target = call<CreateTarget>(*rendering, {targets[0].description, abandoned_surface});
        call<Attach>(*rendering, {scene, abandoned_view, partial_target.target, failed_program});
        auto partial_image = call<CreateTarget>(*rendering, {image_description});
        auto partial_effect = probeRequest(
            image_description, implementation, ClearConfiguration{image_description}, {"color"}
        );
        const auto partial_program = call<CreateEffect>(*rendering, std::move(partial_effect)).effect;
        effects.push_back(partial_program);
        const auto partial_view = call<CreateView>(*rendering, {scene}).view;
        call<Attach>(*rendering, {scene, partial_view, partial_image.target, partial_program});
        SurfaceDescription partial_surface;
        CHECK(makeWindowSurface(second->nativeWindow(), false, partial_surface));
        auto partial_consumer = call<CreateTarget>(*rendering, {targets[1].description, partial_surface});
        const auto consumer_view = call<CreateView>(*rendering, {scene}).view;
        AttachmentRequest sample_partial{
            scene, consumer_view, partial_consumer.target, make_consumer(partial_consumer.description)
        };
        sample_partial.target_images.push_back({"scene-output", partial_image.target});
        call<Attach>(*rendering, std::move(sample_partial));
        for (unsigned i = 0; i < config.frames_in_flight; ++i) frame();
        {
            // An ordinary RenderSystem drives Engine's real BEGIN/END/ABORT stages. No failure
            // handling is added to the Engine or test System; the transport must stop this run.
            RenderCapabilityMetaRegistry metadata;
            CHECK(metadata.addCapability(cxx::make_basic_type_info<RenderProbeCapability>()));
            auto world = std::make_unique<world::World>(world::WorldDescription{"failure cleanup", {}});
            auto effect = probeRequest(
                image_description, implementation, ClearConfiguration{image_description}, {"color"}
            );
            auto rendering_system = take(RenderSystem::create(
                std::move(effect.composition), effect.output, *world, *context, metadata
            ));
            auto candidate = std::make_unique<scene::SceneInstance>(
                std::move(world), std::make_unique<simulation::Simulation>()
            );
            candidate->add(std::move(rendering_system));
            engine::Engine engine{*context};
            CHECK(engine.addScene(candidate));
            std::vector<FrameCompletion> outcomes;
            unsigned rounds = 0;
            engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
            {
                frames.writer().onCompletion(
                    &outcomes,
                    [](void* state, const FrameCompletion& value) noexcept
                    {
                        static_cast<std::vector<FrameCompletion>*>(state)->push_back(value);
                    }
                );
                if (++rounds == 16) engine.requestStop(); // A failed transport must not run indefinitely.
                return {};
            }, engine::EEngineStage::END_UPDATE);
            reject_submission = 2;
            const auto failed_run = engine.run([]() noexcept { window::LuxWindow::pollEvents(); });
            CHECK(!failed_run && failed_run.error().type == kDeviceFailure);
            CHECK(waitFrameOperation(frames.whenSubmittedFramesConsumed()));
            CHECK(submissions_before_failure == 0 && submission_operations->QueueSubmit == submit_native);
            CHECK(std::ranges::any_of(outcomes, [](const FrameCompletion& outcome) noexcept
            {
                return outcome.error.type == kDeviceFailure && outcome.outcome == EFrameOutcome::PARTIALLY_SUBMITTED &&
                    outcome.submitted_batches > 0 && outcome.requires_resynchronization;
            }));
            // Engine owns and retires its Scene before the metadata/Context borrows expire.
        }
        call<RemoveTarget>(*rendering, partial_target.target);
        call<RemoveTarget>(*rendering, partial_consumer.target);
        call<RemoveTarget>(*rendering, partial_image.target);
    }
    else
    {
        // Preparation itself requires ResourceUploader. Without it this fixture deliberately
        // verifies contract rejection and unused-acquire retirement, not preparation recovery.
        call<RemoveTarget>(*rendering, abandoned.target);
    }
    first.reset();
    second.reset(); // Actual RemoveTarget responses returned both native-window borrows.
    call<RemoveScene>(*rendering, {scene});
    for (auto effect : effects)
    {
        call<RemoveEffect>(*rendering, {effect});
    }
    const auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    context.reset(); // Server, Object and Execution settle in the real EngineContext ownership order.
    CHECK(validation_errors == 0);
    std::printf(
        "Presentation: %llu accepted presents, validation errors=%llu warnings=%llu\n",
        static_cast<unsigned long long>(stats.presentations),
        static_cast<unsigned long long>(stats.validation_errors),
        static_cast<unsigned long long>(stats.validation_warnings)
    );
}
