#pragma once
/**
 * @file MainCloseDriver.hpp
 * @brief The composition root's sole blocking adapter for sender-first close.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/frame/visibility.h>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>

#include <chrono>
#include <functional>

namespace lux::exec { class AsyncScopeCloseSender; class AsyncScope; }
namespace lux::logging { class LogRouterCloseSender; class LogRouter; }
namespace lux::extensions { class EngineExtensionsCloseSender; }

namespace lux::runtime
{
    class FrameCoordinator;
    class SceneRuntimeCloseSender;
    class ResidencyCloseSender;
    class AsyncRenderUploadCloseSender;

    enum class EMainCloseError : std::uint8_t
    {
        WATCHDOG_EXPIRED,
        RUNTIME_JOIN_FAILED
    };

    class LUX_FRAME_RUNTIME_PUBLIC MainCloseDriver final
    {
    public:
        explicit MainCloseDriver(
            FrameCoordinator& coordinator,
            lux::exec::AsyncRuntime& runtime,
            std::chrono::steady_clock::duration watchdog =
                std::chrono::seconds{30}) noexcept;

        [[nodiscard]] lux::cxx::expected<
            SceneCloseReport,
            EMainCloseError>
        close(SceneRuntimeCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<
            SceneCloseReport,
            EMainCloseError>
        close(SceneRuntime& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<
            ResidencyCloseReport,
            EMainCloseError>
        close(ResidencyCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<
            ResidencyCloseReport,
            EMainCloseError>
        close(ResidencyAssembly& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<
            AsyncRenderUploadCloseReport,
            EMainCloseError>
        close(AsyncRenderUploadCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<
            AsyncRenderUploadCloseReport,
            EMainCloseError>
        close(AsyncRenderUploadService& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EMainCloseError>
        close(lux::logging::LogRouterCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EMainCloseError>
        close(lux::logging::LogRouter& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EMainCloseError>
        close(lux::exec::AsyncScopeCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EMainCloseError>
        close(lux::exec::AsyncScope& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<
            lux::extensions::EngineExtensionsCloseReport,
            EMainCloseError>
        close(lux::extensions::EngineExtensionsCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<
            lux::extensions::EngineExtensionsCloseReport,
            EMainCloseError>
        close(lux::extensions::EngineExtensions& owner) noexcept;

        [[nodiscard]] lux::cxx::expected<
            lux::exec::AsyncCloseReport,
            EMainCloseError>
        close(lux::exec::AsyncRuntimeCloseSender sender) noexcept;
        [[nodiscard]] lux::cxx::expected<
            lux::exec::AsyncCloseReport,
            EMainCloseError>
        close(lux::exec::AsyncRuntime& owner) noexcept;

    private:
        FrameCoordinator& coordinator_;
        lux::exec::AsyncRuntime& runtime_;
        std::chrono::steady_clock::duration watchdog_;
    };
}
