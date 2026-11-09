#include "Sample.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/HzbFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <thread>

namespace
{
    using namespace lux::render;

    template <class T> T require(RenderResult<T> value)
    {
        if (!value)
        {
            std::fprintf(stderr, "Render sample error %llu\n", static_cast<unsigned long long>(value.error().type));
            std::abort();
        }
        return std::move(*value);
    }

} // namespace

static RenderResult<CapabilityGraphContribution> lowerClientProbe(const CapabilityBuildInput& input) noexcept
{
    const auto& config = *static_cast<const MeshFeatureConfig*>(input.configuration.data());
    CapabilityGraphContribution result;
    result.definition.features.push_back({input.features[0], "main", take(ConfigValue::make(config))});
    if (input.features.size() == 3)
    {
        HzbFeatureConfig hzb{{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT}, 6};
        hzb.depth.extent.source = EImageExtentSource::TARGET;
        result.definition.features.push_back({input.features[2], "depth pyramid", take(ConfigValue::make(hzb))});
        result.definition.connections.push_back({{0, "depth"}, {1, "depth"}});
    }
    MeshVisibilityFeatureConfig visible;
    visible.view_parameters = meshViewParameterBlock();
    const auto index = static_cast<std::uint32_t>(result.definition.features.size());
    result.definition.features.push_back({input.features[1], "visibility", take(ConfigValue::make(visible))});
    for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
        result.definition.connections.push_back({{index, port}, {0, port}});
    result.ports.push_back({{0, "color"}});
    if (input.features.size() == 3) result.ports.push_back({{0, "depth"}});
    return result;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(
            stderr,
            "usage: render_basic_mesh <CookedTexturedUnlit.frag.spv> "
            "[--paged] [--single-queue|--same-family] [--fif=1|2|3]\n"
        );
        return 2;
    }
    auto created = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    if (!created)
    {
        return 3;
    }
    auto execution = std::move(*created);
    DeviceOptions options{};
    RenderServerConfig config{"example.offscreen3d", true};
    options.texture_descriptors = true;
    options.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view argument{argv[i]};
        if (argument == "--paged")
        {
            options.buffer_device_address = true;
        }
        else if (argument == "--single-queue")
        {
            options.multiple_queues = false;
        }
        else if (argument == "--same-family")
        {
            options.prefer_dedicated_queues = false;
        }
        else if (argument == "--fif=1" || argument == "--fif=2" || argument == "--fif=3")
        {
            config.frames_in_flight = static_cast<std::uint32_t>(argument.back() - '0');
        }
        else
        {
            return 2;
        }
    }
    auto server = require(RenderServer::create(config, *execution, options));
    const std::array graph_features{
        require(server->featureId("lux.render.feature.MaterialMeshRaster")),
        require(server->featureId("lux.render.feature.MeshDepthRaster")),
        require(server->featureId("lux.render.feature.Hzb")),
        require(server->featureId("lux.render.feature.MeshVisibilityFeature"))
    };

    const std::array implementations{
        registerRenderProbe<MeshFeatureConfig>(
            *server, "test.client.color", {graph_features[0], graph_features[3]},
            probePorts({"color"}, {}), kRasterProbeDependencies, lowerClientProbe
        ),
        registerRenderProbe<MeshFeatureConfig>(
            *server, "test.client.depth", {graph_features[1], graph_features[3], graph_features[2]},
            probePorts({"color", "depth"}, {}), kRasterProbeDependencies, lowerClientProbe
        )
    };
    auto main_client = lux::platform::ipc::IpcCore::instance().createRequester(config.channel_name);
    if (!main_client || !server->setFrameSource(main_client->id()))
    {
        return 4;
    }
    bool ran = false;
    std::thread render(
        [&]
        {
            auto result = server->run();
            ran = bool(result);
            if (!result)
            {
                std::fprintf(
                    stderr,
                    "Render loop failed error=%llu argument=%llu\n",
                    static_cast<unsigned long long>(result.error().type),
                    static_cast<unsigned long long>(result.error().args[0])
                );
            }
        }
    );
    mesh_sample::runClients(std::move(*main_client), config.channel_name, argv[1], implementations);
    server->stop();
    render.join();
    const auto stats = server->stats();
    if (!ran || stats.validation_errors)
    {
        std::abort();
    }
    server.reset();
    execution.reset();
    std::printf(
        "3D sample: two Clients, one ExecutionRuntime, one Mesh/Material shared, typed Camera parameters and "
        "sealed frame rendering; "
        "graphs=%llu submissions=%llu validation=%llu warnings=%llu\n",
        static_cast<unsigned long long>(stats.compiled_graphs),
        static_cast<unsigned long long>(stats.submissions),
        static_cast<unsigned long long>(stats.validation_errors),
        static_cast<unsigned long long>(stats.validation_warnings)
    );
}
