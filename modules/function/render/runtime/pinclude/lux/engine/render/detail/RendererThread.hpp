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
        explicit RendererThread(const RendererConfig& config)
            : frames(TRenderProgramChannel<>::create(config.frame_capacity)),
              controls(TRenderControlChannel<>::create(config.control_capacity)),
              uploads(TRenderUploadChannel<>::create(config.upload_capacity, config.upload_byte_capacity)),
              sync(std::make_shared<RenderChannelSync>()), statistics(std::make_shared<RenderStatistics>())
        {}
        std::shared_ptr<lux::render::TRenderProgramChannel<>> frames;
        std::shared_ptr<lux::render::TRenderControlChannel<>> controls;
        std::shared_ptr<lux::render::TRenderUploadChannel<>> uploads;
        std::shared_ptr<lux::render::RenderChannelSync> sync;
        std::shared_ptr<RenderStatistics> statistics;
        lux::render::FeatureCatalog catalog;
        std::atomic<unsigned> startup{}, stopped{};
        lux::render::RenderError startup_error;
    };
    RenderResult<std::jthread> startRendererThread(RendererThread&, const RendererConfig&, ValidationMessageSink);
} // namespace lux::render::detail
