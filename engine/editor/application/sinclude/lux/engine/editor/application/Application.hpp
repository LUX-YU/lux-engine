#pragma once
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/editor/application/tooling/Toolset.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <optional>
namespace lux::editor::application
{
    enum class EApplicationState : std::uint8_t
    {
        COMPOSING,
        STARTING,
        RUNNING,
        START_FAILED,
        CLOSE_REQUESTED,
        DETACHING_UI,
        CLOSING_VIEWS,
        CLOSING_SESSIONS,
        DRAINING_RENDERER,
        STOPPED
    };
    enum class EApplicationError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        INVALID_STATE,
        BUSY,
        START_FAILURE,
        WINDOW_FAILURE,
        SCENE_FAILURE,
        RENDERER_FAILURE,
        ALLOCATION_FAILURE,
        PROCESS_FAILURE
    };
    struct ApplicationFailure final
    {
        EApplicationError code{};
        std::optional<ui::WindowFailure> window;
        std::optional<sessions::SceneFailure> scene;
        std::optional<rendering::RendererFailure> renderer;
        std::optional<rendering::RendererDiagnostic> renderer_diagnostic;
        std::optional<lux::process::EExecutionError> execution;
        std::optional<lux::process::asset_loading::EVfsAssetReadEndpointError> assets;
    };
    template <class T> using ApplicationResult = lux::cxx::expected<T, ApplicationFailure>;
    // A cold example/project factory, called once during start. It never reaches a Pane or a frame callback.
    using InspectionSourceFactory = sessions::SceneResult<sessions::SceneOpenInfo> (*)(
        sessions::SessionId, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>) noexcept;
    struct EditorApplicationCreateInfo final
    {
        ui::WindowSpec window;
        rendering::RendererConfig renderer;
        lux::process::ExecutionRuntimeConfig execution;
        lux::process::asset_loading::VfsAssetReadEndpointConfig asset_read;
        std::vector<lux::asset::MountDesc> mounts;
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
        InspectionSourceFactory source{};
    };
    struct ApplicationShutdownStatus final
    {
        EApplicationState state{};
        bool close_requested{}, workspace_present{}, unattached_view_present{};
        std::optional<rendering::RendererCloseStatus> renderer;
        std::shared_ptr<const sessions::SceneCloseSnapshot> session;
    };
    class EditorApplication final
    {
    public:
        [[nodiscard]] static ApplicationResult<std::unique_ptr<EditorApplication>> create(
            EditorApplicationCreateInfo &) noexcept;
        [[nodiscard]] ApplicationResult<void> start() noexcept;
        // Cold composition only. Retained borrows still obey Toolset's freeze/stop admission checks.
        [[nodiscard]] ApplicationResult<std::reference_wrapper<Toolset>> tooling() noexcept;
        [[nodiscard]] ApplicationResult<std::size_t> run(std::size_t max_frames = 0) noexcept;
        // Non-result queries require this application's owner thread; shutdownStatus() rejects foreign threads.
        [[nodiscard]] EApplicationState state() const noexcept;
        [[nodiscard]] ApplicationResult<void> requestClose() noexcept;
        [[nodiscard]] ApplicationResult<bool> advanceShutdown(std::size_t budget) noexcept;
        [[nodiscard]] ApplicationResult<ApplicationShutdownStatus> shutdownStatus() const noexcept;
        [[nodiscard]] rendering::RendererStatistics rendererStatistics() const noexcept;
        ~EditorApplication() noexcept;
        EditorApplication(const EditorApplication &) = delete;
        EditorApplication &operator=(const EditorApplication &) = delete;
        EditorApplication(EditorApplication &&) = delete;
        EditorApplication &operator=(EditorApplication &&) = delete;

    private:
        struct Impl;
        explicit EditorApplication(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::application
