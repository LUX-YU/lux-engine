#pragma once
#include <lux/engine/editor/FrameworkError.hpp>
#include <lux/engine/ui/Root.hpp>
#include <memory>

namespace lux::editor
{
    class EditorUIRoot final : public ui::Root
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorUIRoot>> create(
            ui::RootConfig = {}
        ) noexcept;
        ~EditorUIRoot() noexcept override;
        EditorUIRoot(const EditorUIRoot&) = delete;
        EditorUIRoot& operator=(const EditorUIRoot&) = delete;
        EditorUIRoot(EditorUIRoot&&) = delete;
        EditorUIRoot& operator=(EditorUIRoot&&) = delete;

        [[nodiscard]] FrameworkResult<void> mountProjectUi(std::span<std::unique_ptr<ui::Pane>>) noexcept;
        [[nodiscard]] FrameworkResult<ui::PaneHandle> takePane(std::unique_ptr<ui::Pane>&) noexcept;
        [[nodiscard]] ui::Pane* projectPane(const ui::PaneHandle&) const noexcept;
        [[nodiscard]] std::size_t projectPaneCount() const noexcept;
        [[nodiscard]] FrameworkResult<void> removePane(const ui::PaneHandle&) noexcept;
        [[nodiscard]] FrameworkResult<void> clearProjectUi() noexcept;
        [[nodiscard]] FrameworkResult<void> checkStructureSafe() noexcept;

        using Capture = cxx::function_ref<cxx::expected<void, ui::ECaptureError>(const ui::DrawData&)>;
        // Capture is borrowed only for this invocation, before Root routes input or maintains owners.
        [[nodiscard]] cxx::expected<void, ui::ECaptureError> frame(ui::FrameInfo, ui::DrawData*, Capture) noexcept;

    private:
        EditorUIRoot() noexcept;
        [[nodiscard]] cxx::expected<void, ui::ECaptureError> drawDataReady(const ui::DrawData&) noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
