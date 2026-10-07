#pragma once
#include <lux/engine/editor/EditorAssembly.hpp>
#include <lux/engine/editor/EditorConfig.hpp>
#include <lux/engine/editor/FrameStatistics.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <memory>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    enum class EFrameStatus : std::uint8_t
    {
        RUNNING,
        EXIT_REQUESTED
    };

    class LuxEngine final : public object::LuxObject
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<LuxEngine>> create(
            EditorConfig = {},
            EditorAssembly = {}
        ) noexcept;
        ~LuxEngine() noexcept override;
        LuxEngine(const LuxEngine&) = delete;
        LuxEngine& operator=(const LuxEngine&) = delete;
        LuxEngine(LuxEngine&&) = delete;
        LuxEngine& operator=(LuxEngine&&) = delete;

        [[nodiscard]] FrameworkResult<void> exec() noexcept;
        // One host iteration; callers do not drive SceneRuntime a second time.
        [[nodiscard]] FrameworkResult<EFrameStatus> frame() noexcept;
        [[nodiscard]] FrameStatistics statistics() const noexcept;
        [[nodiscard]] EditorWindow& window() noexcept;
        [[nodiscard]] engine::EngineContext& engine() noexcept;
        [[nodiscard]] const engine::EngineContext& engine() const noexcept;
        [[nodiscard]] EditorContext* project() noexcept;
        [[nodiscard]] const EditorContext* project() const noexcept;

        object::TSignal<> projectChanged{*this};
        object::TSignal<ProjectOpenFailure> projectOpenFailed{*this};

    protected:
        void event(object::EventView&) noexcept override;

    private:
        struct Impl;
        explicit LuxEngine(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
