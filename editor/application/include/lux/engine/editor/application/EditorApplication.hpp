#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <filesystem>
#include <lux/engine/window/WindowPlacement.hpp>
#include <lux/engine/editor/commands/Command.hpp>

namespace lux::editor::workspace
{
    struct DockLayout;
}

namespace lux::editor::application
{
    struct EditorApplicationConfig final
    {
        std::filesystem::path project_file;
        std::filesystem::path installation;
        std::string title{"Lux Editor"};
        std::optional<int> width, height;
        // Uses the same desktop and renderer with an offscreen output, without a native window.
        bool offscreen{};
        std::optional<std::filesystem::path> font;
        // Host override for isolated profiles; defaults to the platform user config directory.
        std::optional<std::filesystem::path> user_directory;
        std::optional<window::EWindowMode> window_mode;
        std::optional<float> scale;
    };
    enum class EApplicationPhase : std::uint8_t
    {
        RUNNING,
        REVIEWING,
        COMMITTING_EXIT,
        DRAINING,
        RELEASED
    };
    struct OpenAndShowResult final
    {
        sessions::OpenAssetStatus content;
        std::optional<views::ViewId> view;
        std::optional<EditorFailure> presentation_failure;
    };
    class EditorApplication final
    {
    public:
        [[nodiscard]] static EditorResult<std::unique_ptr<EditorApplication>> create(EditorApplicationConfig);
        ~EditorApplication();
        EditorApplication(const EditorApplication&) = delete;
        EditorApplication& operator=(const EditorApplication&) = delete;
        EditorApplication(EditorApplication&&) = delete;
        EditorApplication& operator=(EditorApplication&&) = delete;
        [[nodiscard]] EditorResult<void> exec();
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EApplicationPhase phase() const noexcept;
        [[nodiscard]] EditorResult<sessions::OpenAssetId> open(AssetReference);
        [[nodiscard]] EditorResult<OpenAndShowResult> openStatus(sessions::OpenAssetId) const;
        [[nodiscard]] EditorResult<void> cancelOpen(sessions::OpenAssetId);
        [[nodiscard]] EditorResult<void> acknowledgeOpen(sessions::OpenAssetId);
        [[nodiscard]] EditorResult<views::ViewId> show(sessions::SessionId, bool another_view = false);
        [[nodiscard]] EditorResult<void> requestExit();
        [[nodiscard]] EditorResult<void> closeView(views::ViewId);
        [[nodiscard]] EditorResult<void> applyLayout(workspace::DockLayout);
        [[nodiscard]] commands::CommandResult<commands::DispatchReceipt> execute(
            commands::CommandId,
            commands::CommandInvocation = commands::CommandInvocation{}
        );

    private:
#if defined(LUX_APPLICATION_TEST_ACCESS)
        friend struct ApplicationTestAccess;
#endif
        struct Impl;
        explicit EditorApplication(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
