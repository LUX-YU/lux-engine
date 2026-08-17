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

    template <typename T>
    using MainCloseExp = lux::cxx::expected<T, EMainCloseError>;

    class LUX_FRAME_RUNTIME_PUBLIC MainCloseDriver final
    {
    public:
        explicit MainCloseDriver(
            FrameCoordinator& coordinator,
            lux::exec::AsyncRuntime& runtime,
            std::chrono::steady_clock::duration watchdog =
                std::chrono::seconds{30}) noexcept;

        [[nodiscard]] MainCloseExp<SceneCloseReport>
        close(SceneRuntimeCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<SceneCloseReport>
        close(SceneRuntime& owner) noexcept;

        [[nodiscard]] MainCloseExp<ResidencyCloseReport>
        close(ResidencyCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<ResidencyCloseReport>
        close(ResidencyAssembly& owner) noexcept;

        [[nodiscard]] MainCloseExp<AsyncRenderUploadCloseReport>
        close(AsyncRenderUploadCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<AsyncRenderUploadCloseReport>
        close(AsyncRenderUploadService& owner) noexcept;

        [[nodiscard]] MainCloseExp<void>
        close(lux::logging::LogRouterCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<void>
        close(lux::logging::LogRouter& owner) noexcept;

        [[nodiscard]] MainCloseExp<void>
        close(lux::exec::AsyncScopeCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<void>
        close(lux::exec::AsyncScope& owner) noexcept;

        [[nodiscard]] MainCloseExp<lux::extensions::EngineExtensionsCloseReport>
        close(lux::extensions::EngineExtensionsCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<lux::extensions::EngineExtensionsCloseReport>
        close(lux::extensions::EngineExtensions& owner) noexcept;

        [[nodiscard]] MainCloseExp<lux::exec::AsyncCloseReport>
        close(lux::exec::AsyncRuntimeCloseSender sender) noexcept;
        [[nodiscard]] MainCloseExp<lux::exec::AsyncCloseReport>
        close(lux::exec::AsyncRuntime& owner) noexcept;

    private:
        FrameCoordinator& coordinator_;
        lux::exec::AsyncRuntime& runtime_;
        std::chrono::steady_clock::duration watchdog_;
    };
}
