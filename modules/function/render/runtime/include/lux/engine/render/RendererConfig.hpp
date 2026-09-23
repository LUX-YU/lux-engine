#pragma once
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>
#include <lux/engine/render/runtime/visibility.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace lux::render
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
    std::size_t scene_capacity{128};
    std::size_t diagnostic_capacity{64};
    lux::render::ProgramMemoryHints program_memory{32, 8192, 2, 8, 1024};
    std::vector<std::string> instance_extensions;
    bool validation{};
};
struct SampledOutput final
{
};
struct NativeSurfaceOutput final
{
    std::uint64_t native_window{}; // Platform handle; the owner keeps it alive through target retirement.
};
struct ViewConfig final
{
    PixelExtent extent;
    std::variant<SampledOutput, NativeSurfaceOutput> output;
    double coordinate_page_size{1024.0}; // Immutable configuration of the associated RenderScene.
};
struct RendererStatistics final
{
    std::uint64_t frames{};
    std::uint64_t render_events{}, dropped_events{}, gpu_completed{};
    std::size_t runtime_leases{}, views{}, accepted_frames{};
    std::uint64_t validation_errors{};
};
} // namespace lux::render
