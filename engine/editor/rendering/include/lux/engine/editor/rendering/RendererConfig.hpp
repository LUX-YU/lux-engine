#pragma once
#include <lux/engine/editor/rendering/visibility.h>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>
#include <lux/engine/ui/TextureHandle.hpp>
#include <array>
#include <memory>
#include <span>
#include <functional>
#include <optional>
namespace lux::editor::rendering
{
    struct PixelExtent final
    {
        std::uint32_t width{}, height{};
        friend constexpr bool operator==(PixelExtent, PixelExtent) noexcept = default;
    };
    struct RenderViewId final
    {
        std::uint64_t renderer{}, slot{}, generation{};
        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return renderer && generation;
        }
        friend constexpr bool operator==(RenderViewId, RenderViewId) noexcept = default;
    };
    struct ViewStamp final
    {
        std::uint64_t session{}, source_revision{}, view_revision{}, surface_generation{};
        friend constexpr bool operator==(const ViewStamp &, const ViewStamp &) noexcept = default;
    };
    enum class EImageEvidence : std::uint8_t
    {
        REQUESTED,
        RECORDED,
        GPU_COMPLETE
    };
    // GPU_COMPLETE is NOT proof of physical screen presentation.
    struct ImageContentStamp final
    {
        ViewStamp source;
        std::uint64_t frame_serial{}; // Zero when no actual recorded frame is known.
        EImageEvidence evidence{EImageEvidence::REQUESTED};
    };
    enum class ERendererError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        NOT_READY,
        BUSY,
        STOPPING,
        CAPACITY,
        ALLOCATION_FAILURE,
        DEVICE_FAILURE,
        STALE_VIEW,
        STALE_IMAGE,
        INCOMPLETE_FRAME_REFERENCES,
        EXTERNAL_FAILURE,
        CONTRACT_FAILURE
    };
    struct RendererFailure final
    {
        ERendererError code{};
        lux::render::RenderError render_error{};
        RenderViewId view;
        std::uint64_t request{};
        std::optional<std::uint32_t> backend_status;
    };
    struct RendererDiagnostic final
    {
        RendererFailure failure;
        // Keep the backend's actual identity. A scene slot is not a Session or full Scene generation.
        std::uint32_t scene_index{lux::render::RenderErrorEvent::kNoScene}, occurrences{1};
        std::uint64_t frame_serial{}, backend_sequence{};
        bool terminal{};
    };
    template <class T> using RenderResult = lux::cxx::expected<T, RendererFailure>;
    enum class ERendererState : std::uint8_t
    {
        STARTING,
        READY,
        STOPPING,
        STOPPED,
        FAILED
    };
    enum class EViewState : std::uint8_t
    {
        CREATING,
        READY,
        RESIZING,
        SUSPENDED,
        CLOSING,
        CLOSED,
        FAILED
    };
    enum class EFrameSubmit : std::uint8_t
    {
        SUBMITTED,
        BACKPRESSURED
    };
    enum class ERenderClose : std::uint8_t
    {
        PENDING,
        COMPLETE
    };
    struct RendererConfig final
    {
        std::size_t frame_capacity{3}, control_capacity{8}, upload_capacity{8}, upload_byte_capacity{16 * 1024 * 1024};
        std::size_t view_capacity{8}, texture_capacity{48};
        std::size_t diagnostic_capacity{64};
        lux::render::ProgramMemoryHints program_memory{32, 8192, 2, 8, 1024};
        bool validation{};
        std::function<void(std::uint32_t, std::string_view)> validation_message_sink;
    };
    struct ViewConfig final
    {
        PixelExtent extent;
        bool sampled{true};
        double coordinate_page_size{1024.0}; // Immutable configuration of the associated RenderScene.
    };
    struct CameraFrame final
    {
        std::array<double, 16> view{}, projection{};
        std::array<double, 3> origin{};
        ViewStamp desired; // A request, not an observed completed image.
    };

    struct RendererStatistics final
    {
        std::uint64_t frames{}, slots{}, descriptors_created{}, descriptors_retired{}, texture_misses{};
        std::uint64_t render_events{}, dropped_events{}, gpu_completed{};
        std::size_t runtime_leases{}, views{}, accepted_frames{};
        std::uint64_t validation_errors{};
    };
} // namespace lux::editor::rendering
