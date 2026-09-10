#pragma once
// Non-installed diagnostics. Faults are attached to an actual owning packet before real queue admission.
#include <lux/engine/editor/rendering/EditorFramePacket.hpp>
namespace lux::editor::rendering
{
    class EditorRenderer;
}
extern "C" LUX_EDITOR_RENDERING_PUBLIC void lux_er1_renderer_allocation_fail_after(std::size_t) noexcept;
extern "C" LUX_EDITOR_RENDERING_PUBLIC std::size_t lux_er1_renderer_allocation_disarm() noexcept;
namespace lux::editor::rendering::detail
{
    enum class EStartupFault : unsigned
    {
        NONE,
        THREAD_LAUNCH,
        INITIALIZE,
        AFTER_DEVICE,
        AFTER_ATTACH
    };
    struct StartupTrace final
    {
        unsigned workers_started{}, servers_created{}, initialized{}, attached{}, servers_destroyed{}, workers_exited{};
    };
    struct ResourceMemoryTrace final
    {
        std::uint64_t samples{}, peak_bytes{}, peak_allocations{}, last_bytes{}, last_allocations{};
    };
    struct SharedImportTrace final
    {
        std::uint64_t view_pairs{}, write_sample_barriers{}, cross_view_barriers{}, recorded_reads{}, recorded_writes{};
        std::uint64_t first_serial{}, image_identity{};
    };
    struct LUX_EDITOR_RENDERING_PUBLIC RendererTestAccess final
    {
        // Cold, test-process-only configuration; call only after the previous worker has joined.
        // INITIALIZE uses an unsupported Vulkan extension; other stages return the supplied injected error.
        static void failStartup(EStartupFault, lux::render::RenderError) noexcept;
        static StartupTrace startupTrace() noexcept;
        static RenderResult<void> rejectMaterialUpload(
            EditorRenderer &, lux::render::ShaderHandle forward, lux::render::ShaderHandle gbuffer) noexcept;
        static RenderResult<std::uint64_t> rejectedMaterialUploads(const EditorRenderer &) noexcept;
        static RenderResult<void> observeResourceMemory(EditorRenderer &) noexcept;
        static RenderResult<ResourceMemoryTrace> resourceMemoryTrace(const EditorRenderer &) noexcept;
        static RenderResult<void> observeSharedImports(EditorRenderer &) noexcept;
        static RenderResult<SharedImportTrace> sharedImportTrace(const EditorRenderer &) noexcept;
        static RenderResult<void> failRecord(EditorFramePacket &, lux::render::RenderError) noexcept;
        // Observe the real admitted request, without delaying or replacing its reply.
        static std::uint64_t inFlightResize(const RenderView &) noexcept;
        static void useSceneForNextView(lux::render::RenderSceneId) noexcept;
        static RenderResult<void> pauseConsumer(EditorRenderer &, bool) noexcept;
        static bool consumerPaused(const EditorRenderer &) noexcept;
    };
} // namespace lux::editor::rendering::detail
