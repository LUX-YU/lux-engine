#include "Sample.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/features/MeshFeature.hpp>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{
    using namespace lux::render;

    template <class T> T require(RenderResult<T> value)
    {
        if (!value)
        {
            std::fprintf(stderr, "Host Render error %llu\n", static_cast<unsigned long long>(value.error().type));
            std::abort();
        }
        return std::move(*value);
    }

    RenderError mapCpuError(lux::execution::EExecutionError error) noexcept
    {
        return {kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}};
    }

    // An actual second Command implementation installed by this application, not a Server switch branch.
    // It counts accepted dispatches and delegates Native work to the same supported Texture algorithm.
    class AlternateTexture final
    {
    public:
        static constexpr std::string_view name = "example.TextureB";
        static inline unsigned calls = 0; // Written only by Render owner, observed after join.
        explicit AlternateTexture(vulkan::DeviceSession& device) noexcept : texture_(device) {}

        RenderResult<void> set(const TextureWrite& value, Reply<SetTexture>& reply) noexcept
        {
            ++calls;
            return texture_.set(value, reply);
        }

        using Bindings = std::tuple<CommandMethod<SetTexture, &AlternateTexture::set>>;

    private:
        vulkan::TextureCommands texture_;
    };

    struct AlternateProvider final
    {
        using Handler = AlternateTexture;
        static StorageFactory factory() noexcept { return vulkan::StorageProvider<TextureStorage>::factory(); }
        static StorageCppType implementationType() noexcept
        { return vulkan::StorageProvider<TextureStorage>::implementationType(); }
    };
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: render_basic_mesh <CookedTexturedUnlit.frag.spv>\n");
        return 2;
    }
    auto created = lux::execution::ExecutionRuntime::create({2, 128, 128, {128}});
    if (!created) return 3;
    auto execution = std::move(*created);
    auto scheduler = execution.cpu();
    RenderServerConfig config{"example.offscreen3d", true};
    config.device.texture_descriptors = true;
    config.device.multiple_queues = true;
    config.cpu = CpuSchedule::bind<mapCpuError>(scheduler);
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
    auto server = require(RenderServer::create(config));
    (void)require(server->registerFeature<BasicMeshFeature>());
    (void)require(server->registerFeature<DepthOnlyFeature>());
    (void)require(server->registerStorage<TextureStorage, AlternateProvider>({}, "B"));

    bool ran = false;
    std::thread render([&] { ran = bool(server->run()); });
    auto retained = mesh_sample::runClients(config.channel_name, argv[1]);
    server->stop();
    render.join();
    const auto stats = server->stats();
    if (!ran || stats.validation_errors || AlternateTexture::calls != 1) std::abort();
    server.reset();
    execution.requestStop();
    if (!execution.join()) std::abort();
    if (retained.bytes().view().size() != 64 * 64 * 4) std::abort();
    std::printf("3D sample: two Clients, one ExecutionRuntime, one Mesh/Material shared, Camera/color/depth verified; "
        "Texture B=%u graphs=%llu submissions=%llu validation=%llu warnings=%llu; late output bytes=%zu\n",
        AlternateTexture::calls, static_cast<unsigned long long>(stats.compiled_graphs),
        static_cast<unsigned long long>(stats.submissions), static_cast<unsigned long long>(stats.validation_errors),
        static_cast<unsigned long long>(stats.validation_warnings), retained.bytes().view().size());
}
