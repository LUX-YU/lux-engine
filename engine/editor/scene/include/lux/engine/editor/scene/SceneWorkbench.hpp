#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/editor/scene/SceneViewRenderPort.hpp>

#include <cstdint>
#include <memory>

namespace lux::editor::workbench
{
    namespace detail { struct SceneWorkbenchDiagnostics; }
    enum class EWorkbenchError : std::uint8_t
    {
        SCHEMA_BUILD_FAILURE,
        META_BUILD_FAILURE,
        WORLD_BUILD_FAILURE,
        SIMULATION_BUILD_FAILURE,
        SCENE_DESCRIPTION_BUILD_FAILURE,
        SCENE_BUILD_FAILURE,
        BINDING_BUILD_FAILURE,
        PANE_REGISTRATION_FAILURE,
        SELECTION_FAILURE,
        ALLOCATION_FAILURE,
    };

    [[nodiscard]] lux::cxx::expected<scene::SceneMetaManager, EWorkbenchError>
    LUX_EDITOR_SCENE_PUBLIC buildDevelopmentSceneMeta() noexcept;

    /** Owns the explicitly non-project development scene and its Inspector pane. */
    class LUX_EDITOR_SCENE_PUBLIC SceneWorkbench final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<SceneWorkbench>, EWorkbenchError>;

        [[nodiscard]] static CreateResult create(EditorContext& context, SceneViewRenderPort& render) noexcept;

        SceneWorkbench(const SceneWorkbench&) = delete;
        SceneWorkbench& operator=(const SceneWorkbench&) = delete;
        SceneWorkbench(SceneWorkbench&&) = delete;
        SceneWorkbench& operator=(SceneWorkbench&&) = delete;
        ~SceneWorkbench() noexcept;
        void beforeUiFrame();
        void afterUiFrame(double seconds, lux::ui::Vec2 framebuffer_scale);
        void requestClose() noexcept;
        [[nodiscard]] bool closeRequested() const noexcept;
        [[nodiscard]] bool advanceClose();

    private:
        friend struct detail::SceneWorkbenchDiagnostics;
        struct Impl;
        explicit SceneWorkbench(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::workbench
