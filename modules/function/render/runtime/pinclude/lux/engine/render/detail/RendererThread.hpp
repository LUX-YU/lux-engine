#pragma once
#include <lux/engine/render/RenderRuntime.hpp>

#include <atomic>
#include <thread>

namespace lux::render::detail
{
struct RenderStatistics final
{
    std::atomic<int> validation_errors{};
    std::atomic<std::uint64_t> frames{}, events{}, dropped{}, completed{};
};
struct RendererThread final
{
    explicit RendererThread(const RendererConfig &config)
        : frames(RenderProgramChannel<>::create(config.frame_capacity)),
          controls(RenderControlChannel<>::create(config.control_capacity)),
          uploads(RenderUploadChannel<>::create(config.upload_capacity, config.upload_byte_capacity)),
          sync(std::make_shared<RenderChannelSync>()), statistics(std::make_shared<RenderStatistics>())
    {
    }
    std::shared_ptr<lux::render::RenderProgramChannel<>> frames;
    std::shared_ptr<lux::render::RenderControlChannel<>> controls;
    std::shared_ptr<lux::render::RenderUploadChannel<>> uploads;
    std::shared_ptr<lux::render::RenderChannelSync> sync;
    std::shared_ptr<RenderStatistics> statistics;
    lux::render::FeatureCatalog catalog;
    std::atomic<unsigned> startup{}, stopped{};
    lux::render::RenderError startup_error;
};
RenderResult<std::jthread> startRendererThread(RendererThread &, const RendererConfig &);
} // namespace lux::render::detail
