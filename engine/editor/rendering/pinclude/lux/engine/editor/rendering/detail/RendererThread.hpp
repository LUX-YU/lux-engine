#pragma once
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>
#include <atomic>
#include <thread>
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#endif

namespace lux::editor::rendering::detail
{
    struct RenderStatistics final
    {
        std::atomic<int> validation_errors{};
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
        std::atomic<bool> observe_shared{}, observe_memory{};
        std::atomic<ResourceMemoryTrace> memory_trace{};
        std::atomic<bool> reject_material{};
        lux::render::ShaderHandle rejected_forward{}, rejected_gbuffer{};
        std::atomic<std::uint64_t> material_rejections{};
        std::atomic<std::uint64_t> shared_pairs{}, shared_write_sample{}, shared_cross_view{};
        std::atomic<std::uint64_t> shared_reads{}, shared_writes{};
        std::atomic<std::uint64_t> shared_first_serial{}, shared_image{};
#endif
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
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
        std::atomic<bool> pause_requested{}, pause_reached{};
#endif
        lux::render::RenderError startup_error;
        lux::render::TypeId submit_operation{};
    };
    RenderResult<std::jthread> startRendererThread(RendererThread &, lux::window::LuxWindow &,
                                                   lux::ui::detail::UiFontAtlasSnapshot,
                                                   const RendererConfig &) noexcept;
} // namespace lux::editor::rendering::detail
