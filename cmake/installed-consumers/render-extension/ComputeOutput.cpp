#include "GraphFill.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>

namespace
{
    namespace graph = lux::graph;

    // One 64-texel RGBA8 row matches the shared capture fixture's minimum pitch.
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr std::uint32_t row_pitch = width * 4;
    constexpr std::uint32_t readback_bytes = row_pitch * height;
    constexpr auto color = effectPortSemantic("color");
    constexpr auto pixels = effectPortSemantic("pixels");
    constexpr std::string_view compute_node = "test.sdk.compute_color";
    constexpr std::string_view capture_node = "test.sdk.compute_capture";

    struct ComputeCaptureDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.compute_readback"}, 0, 0, {}, "Compute output readback.", EResourceDomain::VIEW
        };
    };

    struct ComputeCapture final
    {
        using Descriptor = ComputeCaptureDescriptor;
        static inline CapturedPixels result;
        CaptureBuffer buffer;

        explicit ComputeCapture(ResourceCreateContext& context) noexcept : buffer(context) {}

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps binding{
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection != 0) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport{GraphBufferDesc{readback_bytes}, {}, EGraphUse::TRANSFER_DESTINATION};
                },
                .resolveGraphImport = [](
                    void* storage,
                    std::uint32_t,
                    const VGraphResourceDesc& description,
                    StorageSubscription& subscription,
                    std::span<ResourceInput> output
                ) noexcept
                {
                    const CaptureConfig config{.result = &result, .bytes = readback_bytes};
                    return CaptureBuffer::resolve(
                        &static_cast<ComputeCapture*>(storage)->buffer, description, &config, subscription, output
                    );
                }
            };
            return makeResourceRegistration<Descriptor, ComputeCapture, &create>(nullptr, &binding);
        }

        static RenderResult<std::unique_ptr<ComputeCapture>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            return std::make_unique<ComputeCapture>(context);
        }
    };

    CapabilityDefinition computeDefinition() noexcept
    {
        return {
            .identity = {graph::nodeTypeId(compute_node), std::string{compute_node}, 1},
            .ports = {{color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::OUTPUT}}
        };
    }

    CapabilityDefinition captureDefinition() noexcept
    {
        return {
            .identity = {graph::nodeTypeId(capture_node), std::string{capture_node}, 1},
            .ports = {
                {color, "color", {"test.sdk.compute_rgba8", 1}, graph::EPinDirection::INPUT, 1, true},
                {pixels, "pixels", {"test.sdk.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
            }
        };
    }

    RenderResult<void> buildCompute(EffectBuildContext& context, std::string_view) noexcept
    {
        GraphImageDesc image{width, height};
        image.extent.source = EImageExtentSource::TARGET;
        auto& builder = context.builder();
        const auto generated = take(builder.declareResource({"compute storage", image}));
        const auto output = take(builder.declareResource({"compute output", image}));
        ComputeWork work;
        work.interface.program = take(builder.addProgram(GraphProgram{kGraphFill.load(context.shaderTarget())}));
        work.interface.resources = {{"generated", {{generated, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{generated};
        const auto dispatch = take(builder.addComputePass("generate compute color", std::move(work)));
        const auto copied = take(builder.addTransferPass(
            "copy compute color", TransferWork{{CopyImage{{dispatch.written.front()}, {{output, 0}}}}}
        ));
        return context.publish(color, {{color, copied.written.front()}});
    }

    RenderResult<void> buildCapture(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto input = context.inputs(color);
        CHECK(input.size() == 1 && input.front().size() == 1 && input.front().front().member == color);
        const auto output = take(context.importResource(
            "capture", {ResourceName{ComputeCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{readback_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
        ));
        const ImageCopyRegion region{0, 0, width, height};
        const auto copied = take(context.builder().addTransferPass("copy to readback", TransferWork{{
            CopyImageBuffer{{input.front().front().version}, {output}, row_pitch, readback_bytes, region}
        }}));
        return context.publish(pixels, {{pixels, copied.written.front()}});
    }

    void registerNode(
        RenderServer& server, const CapabilityDefinition& capability, graph::PinSemanticId output,
        EffectImplementation::Build build
    )
    {
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.identity = {capability.identity.canonical_name, capability.identity.version};
        implementation.capability = capability.identity.id;
        implementation.supported_outputs = {output};
        implementation.initialize = [](EffectInitializationContext&, std::string_view arguments) noexcept
            -> RenderResult<void>
        {
            if (!arguments.empty()) return lux::cxx::unexpected(RenderError{kInvalidConfig});
            return {};
        };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }
}

int main(int argc, char** argv)
{
    const bool d3d12 = argc > 1 && std::string_view{argv[1]} == "d3d12";
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"lux.render.sdk.compute_output", nativeValidationEnabled()};
    config.frames_in_flight = 1;
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, **execution));
    registerNode(*server, computeDefinition(), color, buildCompute);
    registerNode(*server, captureDefinition(), pixels, buildCapture);
    take(server->registerResource<ComputeCapture>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto target = call<CreateTarget>(*client, commands, {session, {width, height, format}}).target;
    const auto readback = call<GetOrCreateResource>(
        *client, commands, {session, ViewRequest{scene, view}, ComputeCaptureDescriptor::descriptor.type}
    ).resource_id;
    CreateEffectRequest effect;
    effect.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*effect.graph};
    const auto compute = take(edit.addNode(computeDefinition(), {std::string{compute_node}, 1}));
    const auto capture = take(edit.addNode(captureDefinition(), {std::string{capture_node}, 1}));
    const auto color_pin = edit.pinId(compute, color);
    CHECK(edit.connect(color_pin, edit.pinId(capture, color)));
    CHECK(edit.setOutput("color", color_pin, true));
    CHECK(edit.setOutput("pixels", edit.pinId(capture, pixels)));
    CHECK(edit.commit());
    const auto created = call<CreateEffect>(*client, commands, std::move(effect)).effect;
    JobBinding binding;
    binding.resources = {{"capture", readback}};
    const auto job = startRenderJob(*client, commands, session, scene, view, target, created, std::move(binding));
    renderFrame(*client, commands, job);
    server->stop();
    worker.join();
    CHECK(server->stats().validation_errors == 0);
    const auto& readback_pixels = ComputeCapture::result;
    CHECK(readback_pixels.ready == 1 && readback_pixels.frames.size() == 1);
    constexpr std::size_t pixel_size = 4;
    const auto data = std::span(readback_pixels.frames.front());
    CHECK(data.size() >= (height - 1) * readback_pixels.pitch + width * pixel_size);
    for (std::size_t y = 0; y < height; ++y)
    {
        for (std::size_t x = 0; x < width; ++x)
        {
            const auto pixel = data.subspan(y * readback_pixels.pitch + x * pixel_size, pixel_size);
            // The shader writes exactly 0.5, halfway between the two nearest UNORM8 values.
            const bool green_matches = pixel[1] == std::byte{127} || pixel[1] == std::byte{128};
            const bool other_channels_match = pixel[0] == std::byte{64} && pixel[2] == std::byte{191} &&
                pixel[3] == std::byte{255};
            if (!green_matches || !other_channels_match)
            {
                std::fprintf(stderr, "Pixel (%zu, %zu) at pitch %u: %u %u %u %u\n", x, y, readback_pixels.pitch,
                    std::to_integer<unsigned>(pixel[0]), std::to_integer<unsigned>(pixel[1]),
                    std::to_integer<unsigned>(pixel[2]), std::to_integer<unsigned>(pixel[3]));
                CHECK(false);
            }
        }
    }
    std::printf("SDK COMPUTE OUTPUT PASS: %s produced %ux%u expected pixels through an external Resource\n",
        d3d12 ? "D3D12" : "Vulkan", width, height);
}
