#include "GraphFill.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>

namespace
{
    // One 64-texel RGBA8 row matches the shared capture fixture's minimum pitch.
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;

    struct ComputeColor final
    {
        using Config = void;
        using Ports = ComputeColor;
        static constexpr FeatureName name{"test.sdk.compute_color"};

        static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
        {
            GraphImageDesc image{width, height};
            image.extent.source = EImageExtentSource::TARGET;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
            return {};
        }

        static RenderResult<void> build(const void*, std::string_view key, FeatureBuildContext& context) noexcept
        {
            GraphImageDesc image{width, height};
            image.extent.source = EImageExtentSource::TARGET;
            auto generated = context.graph.declareResource({featureKey(name, key, "storage"), image});
            if (!generated) return lux::cxx::unexpected(generated.error());
            auto program = context.graph.addProgram(GraphProgram{kGraphFill});
            if (!program) return lux::cxx::unexpected(program.error());
            ComputeWork work;
            work.interface.program = *program;
            work.interface.resources = {{"generated", {{*generated, 0}, {}, EGraphAccess::WRITE}}};
            work.dispatch = ImageDispatchArguments{*generated};
            auto dispatch = context.graph.addComputePass(featureKey(name, key, "dispatch"), std::move(work));
            if (!dispatch) return lux::cxx::unexpected(dispatch.error());
            auto copy = context.graph.addTransferPass(
                featureKey(name, key, "output"),
                TransferWork{{CopyImage{{dispatch->written.front()}, {context.resource("color")}}}}
            );
            if (!copy) return lux::cxx::unexpected(copy.error());
            return context.publish("color", copy->written.front());
        }
    };
}

int main(int argc, char** argv)
{
    const bool d3d12 = argc > 1 && std::string_view{argv[1]} == "d3d12";
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    CapturedPixels pixels;
    RenderServerConfig config{"lux.render.sdk.compute_output"};
    config.frames_in_flight = 1;
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, **execution, options));
    const auto compute = take(server->registerFeature<ComputeColor>());
    take(server->registerResource<CaptureBuffer>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread worker([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto target = call<CreateTarget>(*client, commands, {{width, height, format}}).target;
    CreateEffectRequest effect{{format}};
    effect.definition.features.push_back({compute, "compute"});
    effect.definition.features.push_back({capture, "readback", take(ConfigValue::make(CaptureConfig{&pixels, format}))});
    effect.definition.connections.push_back({{0, "color"}, {1, "color"}});
    effect.definition.outputs = {{{0, "color"}, GraphOutputRequirements{format, 1}}, {{1, "pixels"}, {}}};
    const auto created = call<CreateEffect>(*client, commands, std::move(effect)).effect;
    const auto job = startRenderJob(*client, commands, scene, view, target, created);
    renderFrame(*client, commands, job);
    server->stop();
    worker.join();
    CHECK(pixels.ready == 1 && pixels.frames.size() == 1);
    constexpr std::size_t pixel_size = 4;
    const auto data = std::span(pixels.frames.front());
    CHECK(data.size() >= (height - 1) * pixels.pitch + width * pixel_size);
    for (std::size_t y = 0; y < height; ++y)
    {
        for (std::size_t x = 0; x < width; ++x)
        {
            const auto pixel = data.subspan(y * pixels.pitch + x * pixel_size, pixel_size);
            // The shader writes exactly 0.5, halfway between the two nearest UNORM8 values.
            const bool green_matches = pixel[1] == std::byte{127} || pixel[1] == std::byte{128};
            const bool other_channels_match = pixel[0] == std::byte{64} && pixel[2] == std::byte{191} &&
                pixel[3] == std::byte{255};
            if (!green_matches || !other_channels_match)
            {
                std::fprintf(stderr, "Pixel (%zu, %zu) at pitch %u: %u %u %u %u\n", x, y, pixels.pitch,
                    std::to_integer<unsigned>(pixel[0]), std::to_integer<unsigned>(pixel[1]),
                    std::to_integer<unsigned>(pixel[2]), std::to_integer<unsigned>(pixel[3]));
                CHECK(false);
            }
        }
    }
    std::printf("SDK COMPUTE OUTPUT PASS: %s produced %ux%u expected pixels through an external Resource\n",
        d3d12 ? "D3D12" : "Vulkan", width, height);
}
