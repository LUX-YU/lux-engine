#pragma once
#include <lux/engine/editor/EditorAssembly.hpp>
#include <lux/engine/editor/EditorConfig.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <memory>

namespace lux::engine
{
    class EngineContext;
}

namespace lux::rendering
{
    class RenderExecution;
    class RenderAssets;
} // namespace lux::rendering

namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    class LuxEngine;

    namespace detail
    {
        struct LuxEngineTestAccess;
        struct ProjectStartupRequest;
        [[nodiscard]] FrameworkResult<std::unique_ptr<LuxEngine>> createEditor(
            EditorConfig,
            EditorAssembly,
            ProjectStartupRequest
        ) noexcept;
    } // namespace detail

    class LuxEngine final : public object::LuxObject
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<LuxEngine>> create(
            EditorConfig,
            EditorAssembly = {}
        ) noexcept;
        ~LuxEngine() noexcept override;
        LuxEngine(const LuxEngine&) = delete;
        LuxEngine& operator=(const LuxEngine&) = delete;
        LuxEngine(LuxEngine&&) = delete;
        LuxEngine& operator=(LuxEngine&&) = delete;

        [[nodiscard]] FrameworkResult<void> run() noexcept;
        [[nodiscard]] EditorWindow& window() noexcept;
        [[nodiscard]] engine::EngineContext& engine() noexcept;
        [[nodiscard]] const engine::EngineContext& engine() const noexcept;
        // Application facilities borrowed on Main; project closure never stops these resources.
        [[nodiscard]] rendering::RenderExecution& rendering() noexcept;
        [[nodiscard]] const rendering::RenderExecution& rendering() const noexcept;
        [[nodiscard]] rendering::RenderAssets& renderAssets() noexcept;
        // The currently adopted project, valid throughout the running session. A failed switch preserves it.
        // CloseProjectRequest ends that session; do not borrow its Context during/after shutdown.
        [[nodiscard]] EditorContext& project() noexcept;
        [[nodiscard]] const EditorContext& project() const noexcept;

        object::TSignal<> projectChanged{*this};
        object::TSignal<ProjectOpenFailure> projectOpenFailed{*this};

    protected:
        void event(object::EventView&) noexcept override;

    private:
        enum class ELoopState : std::uint8_t
        {
            RUNNING,
            WAITING,
            STOPPED
        };
        [[nodiscard]] FrameworkResult<ELoopState> pumpOnce() noexcept;
        friend struct detail::LuxEngineTestAccess;
        friend FrameworkResult<std::unique_ptr<LuxEngine>> detail::createEditor(
            EditorConfig,
            EditorAssembly,
            detail::ProjectStartupRequest
        ) noexcept;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<LuxEngine>> createConfigured(
            EditorConfig,
            EditorAssembly,
            detail::ProjectStartupRequest
        ) noexcept;
        struct Impl;
        explicit LuxEngine(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
