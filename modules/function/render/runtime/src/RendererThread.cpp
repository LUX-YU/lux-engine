#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/detail/RendererThread.hpp>
#include <lux/engine/render/renderer/FrameOrchestrator.hpp>

#include <system_error>

namespace lux::render::detail
{
namespace
{
class RuntimeServer final : public GeneralRenderServer
{
  public:
    explicit RuntimeServer(RendererThread &state)
        : GeneralRenderServer(state.frames, state.controls, state.uploads, state.sync), statistics_(*state.statistics)
    {
    }

    bool tick() override
    {
        if (!drainTick())
        {
            return false;
        }
        FrameTickState frame{};
        const auto start = beginRenderTick(frame);
        if (start == ETickStage::Failed)
        {
            return false;
        }
        if (start == ETickStage::NoTarget || start == ETickStage::Skipped)
        {
            const bool continued = stepPendingResourceReleases();
            statistics_.completed.store(gpuCompletedSerial(), std::memory_order_release);
            return continued;
        }
        if (!renderRenderTick(frame))
        {
            return false;
        }
        const bool continued = endRenderTick(frame);
        if (continued && frame.rt.primary_cmd)
        {
            ++statistics_.frames;
        }
        statistics_.completed.store(gpuCompletedSerial(), std::memory_order_release);
        return continued;
    }

  private:
    RenderStatistics &statistics_;
};
} // namespace

RenderResult<std::jthread> startRendererThread(RendererThread &state, const RendererConfig &config, ValidationMessageSink diagnostics)
{
    // Device inputs and diagnostics survive server destruction.
    // No UI Context, Window or SceneInstance is borrowed by the backend.
    try
    {
        return std::jthread([&state, config, diagnostics = std::move(diagnostics)] {
            {
                RuntimeServer server(state);
                ServerConfig server_config;
                for (const auto &extension : config.instance_extensions)
                {
                    server_config.instance_extensions.push_back(extension.c_str());
                }
                server_config.enable_validation = config.validation;
                server_config.validation_error_counter = &state.statistics->validation_errors;
                server_config.gpu_completed_serial = &state.statistics->completed;
                server_config.validation_message_sink = diagnostics;
                auto initialized = server.init(std::move(server_config));
                if (!initialized)
                {
                    state.startup_error = initialized.error();
                }
                state.startup.store(initialized ? 1 : 2, std::memory_order_release);
                state.startup.notify_all();
                if (initialized)
                {
                    while (server.tick())
                    {
                    }
                }
            }
            // Published only after Vulkan owners and the server are gone.
            state.sync->requestStop();
            state.stopped.store(1, std::memory_order_release);
        });
    }
    catch (const std::system_error &)
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::EXTERNAL_FAILURE});
    }
}
} // namespace lux::render::detail
