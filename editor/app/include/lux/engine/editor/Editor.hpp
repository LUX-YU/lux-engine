#pragma once
#include <filesystem>
#include <functional>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/WindowSpec.hpp>
#include <lux/engine/editor/app/visibility.h>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor
{
    class EditorContext;

    struct EditorConfig final
    {
        std::filesystem::path project_file;
        std::filesystem::path plugin_root;
        process::ExecutionRuntimeConfig execution;
        WindowSpec window;
    };

    class LUX_EDITOR_APP_PUBLIC Editor final : public lux::ui::Root
    {
    public:
        using Assembly = EditorResult<void> (*)(lux::ui::Root&, EditorContext&) noexcept;
        [[nodiscard]] static EditorResult<std::unique_ptr<Editor>> create(
            EditorConfig,
            Assembly assemble = nullptr
        ) noexcept;
        ~Editor();
        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        int exec();
        void requestExit() noexcept;
        [[nodiscard]] EditorContext& context() noexcept;
        [[nodiscard]] bool closing() const noexcept;
        void fail(EditorFailure);
        [[nodiscard]] const EditorResult<void>& outcome() const noexcept;

    private:
        friend struct EditorTestAccess;
        friend struct EditorTestFactory;
        struct Impl;
        Editor(EditorConfig, std::unique_ptr<Impl>);

        void event(object::EventView&) noexcept override;
        lux::cxx::expected<void, lux::ui::ECaptureError> drawDataReady(const lux::ui::DrawData&) noexcept override;
        std::unique_ptr<Impl> impl_;
    };
}
