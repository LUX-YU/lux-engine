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
        std::size_t texture_capacity{48};
        std::size_t diagnostic_capacity{64};
        lux::render::ProgramMemoryHints program_memory{32, 8192, 2, 8, 1024};
        std::vector<std::string> instance_extensions;
        bool validation{};
    };
    struct RendererStatistics final
    {
        std::uint64_t frames{};
        std::uint64_t render_events{}, dropped_events{}, gpu_completed{};
        std::size_t accepted_frames{};
        std::uint64_t validation_errors{};
    };
} // namespace lux::render
