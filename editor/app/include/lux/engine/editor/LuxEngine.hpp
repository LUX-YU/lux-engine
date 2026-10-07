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
namespace lux::editor
{
    class EditorWindow;
    class EditorContext;
    namespace detail
    {
        struct LuxEngineTestAccess;
    }

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

        [[nodiscard]] FrameworkResult<void> run() noexcept;
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
        enum class EHostState : std::uint8_t
        {
            RUNNING,
            EXIT_REQUESTED
        };
        [[nodiscard]] FrameworkResult<EHostState> pumpOnce() noexcept;
        friend struct detail::LuxEngineTestAccess;
        struct Impl;
        explicit LuxEngine(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
