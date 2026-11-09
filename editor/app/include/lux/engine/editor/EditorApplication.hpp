#pragma once
#include <lux/engine/EngineOperations.hpp>
#include <lux/engine/editor/EditorAssembly.hpp>
#include <lux/engine/editor/EditorConfig.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <memory>

namespace lux::engine
{
    class Engine;
}

namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    class EditorApplication;

    class EditorApplication final : public object::LuxObject
    {
    public:
        object::TSignal<> projectChanged{*this};
        object::TSignal<ProjectLoadOutcome> projectLoadCompleted{*this};

        // Creates the application shell and admits optional startup loading without an internal event loop.
        // Observe projectLoadCompleted or ready() before borrowing project(); run() performs final cleanup.
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorApplication>>
        create(EditorConfig, EditorAssembly = {}, ProjectStartup = {}) noexcept;
        ~EditorApplication() noexcept override;
        EditorApplication(const EditorApplication&) = delete;
        EditorApplication& operator=(const EditorApplication&) = delete;
        EditorApplication(EditorApplication&&) = delete;
        EditorApplication& operator=(EditorApplication&&) = delete;

        [[nodiscard]] EApplicationState state() const noexcept;

        [[nodiscard]] bool ready() const noexcept
        {
            return state() == EApplicationState::RUNNING;
        }

        // Construction creates facilities; project readiness is delivered asynchronously.
        // Main-only outer driver. Errors are returned after accepted work and rendering cleanup finish.
        // Recursive entry from Object or ordinary Execution callbacks is rejected without starting close.
        [[nodiscard]] FrameworkResult<void> run() noexcept;
        [[nodiscard]] EditorWindow& window() noexcept;
        [[nodiscard]] engine::Engine& engine() noexcept;
        [[nodiscard]] const engine::Engine& engine() const noexcept;
        // The currently adopted project, valid throughout the running session. A failed switch preserves it.
        // CloseProjectRequest ends that session; do not borrow its Context during/after shutdown.
        [[nodiscard]] EditorContext& project() noexcept;
        [[nodiscard]] const EditorContext& project() const noexcept;

    protected:
        void event(object::EventView&) noexcept override;

    private:
        struct Impl;
        explicit EditorApplication(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
