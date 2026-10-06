#pragma once
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <memory>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::ui
{
    class Root;
}
namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    struct EditorConfig final
    {
        std::string title{"LuxEngine"};
        int width{1280};
        int height{800};
        std::uint32_t frame_interval_ms{16};
    };

    class LuxEngine final
    {
    public:
        using Assembly = cxx::function_ref<FrameworkResult<void>(EditorContext&)>;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<LuxEngine>> create(EditorConfig = {}) noexcept;
        ~LuxEngine() noexcept;
        LuxEngine(const LuxEngine&) = delete;
        LuxEngine& operator=(const LuxEngine&) = delete;
        LuxEngine(LuxEngine&&) = delete;
        LuxEngine& operator=(LuxEngine&&) = delete;

        [[nodiscard]] FrameworkResult<void> openProject(ProjectDescription, const EditorLayout&, Assembly) noexcept;
        [[nodiscard]] FrameworkResult<void> closeProject() noexcept;
        [[nodiscard]] FrameworkResult<void> exec() noexcept;
        // One host iteration; callers do not drive SceneRuntime a second time.
        [[nodiscard]] FrameworkResult<bool> frame() noexcept;
        [[nodiscard]] EditorWindow& window() noexcept;
        [[nodiscard]] ui::Root& uiRoot() noexcept;
        [[nodiscard]] engine::EngineContext& engine() noexcept;
        [[nodiscard]] EditorContext* context() noexcept;
        [[nodiscard]] std::uint64_t capturedFrames() const noexcept;

    private:
        struct Impl;
        explicit LuxEngine(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
