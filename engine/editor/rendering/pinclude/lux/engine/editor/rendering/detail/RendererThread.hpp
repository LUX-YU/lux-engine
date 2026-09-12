#pragma once
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>
#include <atomic>
#include <thread>

namespace lux::editor::rendering::detail
{
    struct RenderStatistics final
    {
        std::atomic<int> validation_errors{};
        std::atomic<std::uint64_t> frames{}, slots{}, created{}, retired{}, misses{}, events{}, dropped{}, completed{};
    };
    struct RendererThread final
    {
        std::shared_ptr<lux::render::RenderProgramChannel<>> frames;
        std::shared_ptr<lux::render::RenderControlChannel<>> controls;
        std::shared_ptr<lux::render::RenderUploadChannel<>> uploads;
        std::shared_ptr<lux::render::RenderChannelSync> sync;
        std::shared_ptr<RenderStatistics> statistics;
        lux::render::FeatureCatalog catalog;
        std::atomic<unsigned> startup{}, stopped{};
        std::atomic<bool> allocation_failed{};
        std::atomic<std::uint64_t> failed_packet{};
        lux::render::RenderError startup_error;
        lux::render::TypeId submit_operation{};
    };
    RenderResult<std::jthread> startRendererThread(RendererThread &, lux::window::LuxWindow &,
                                                   lux::ui::detail::UiFontAtlasSnapshot,
                                                   const RendererConfig &) noexcept;
} // namespace lux::editor::rendering::detail
