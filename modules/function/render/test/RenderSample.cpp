#include "Sample.hpp"
#include <cstdio>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>
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

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: render_basic_mesh <CookedTexturedUnlit.frag.spv>\n");
        return 2;
    }
    auto created = lux::execution::ExecutionRuntime::create({2, 128, 128, {128}});
    if (!created)
    {
        return 3;
    }
    auto execution = std::move(*created);
    DeviceOptions options{.dynamic_rendering = true};
    RenderServerConfig config{"example.offscreen3d", true};
    options.texture_descriptors = true;
    options.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
    auto server = require(createRenderServer(config, execution, options));
    (void)require(server->registerFeature<MaterialMeshRasterDescriptor>());
    (void)require(server->registerFeature<MeshDepthRasterDescriptor>());
    (void)require(server->registerStorage<SampledTexture2DStorageDescriptor>("B"));

    const auto directory_id = require(server->commandId<lux::render::ListCommands>());
    bool ran = false;
    std::thread render([&] { ran = bool(server->run()); });
    mesh_sample::runClients(config.channel_name, directory_id, argv[1]);
    server->stop();
    render.join();
    const auto stats = server->stats();
    if (!ran || stats.validation_errors)
    {
        std::abort();
    }
    server.reset();
    execution.requestStop();
    if (!execution.join())
    {
        std::abort();
    }
    std::printf(
        "3D sample: two Clients, one ExecutionRuntime, one Mesh/Material shared, typed Camera parameters and "
        "continuous rendering; "
        "Texture binding B selected; graphs=%llu submissions=%llu validation=%llu warnings=%llu\n",
        static_cast<unsigned long long>(stats.compiled_graphs),
        static_cast<unsigned long long>(stats.submissions),
        static_cast<unsigned long long>(stats.validation_errors),
        static_cast<unsigned long long>(stats.validation_warnings)
    );
}
