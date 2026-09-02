#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

#include <cstdint>
#include <memory>

namespace lux::editor::application
{
    enum class EEditorBootstrapError : std::uint8_t
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

    [[nodiscard]] lux::cxx::expected<scene::SceneMetaManager, EEditorBootstrapError>
    buildDevelopmentSceneMeta() noexcept;

    /** Owns the explicitly non-project development scene and its Inspector pane. */
    class EditorBootstrap final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<EditorBootstrap>, EEditorBootstrapError>;

        [[nodiscard]] static CreateResult create(EditorContext& context) noexcept;

        EditorBootstrap(const EditorBootstrap&) = delete;
        EditorBootstrap& operator=(const EditorBootstrap&) = delete;
        EditorBootstrap(EditorBootstrap&&) = delete;
        EditorBootstrap& operator=(EditorBootstrap&&) = delete;
        ~EditorBootstrap() noexcept;

    private:
        struct Impl;
        explicit EditorBootstrap(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::application
