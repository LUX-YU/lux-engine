#pragma once
#include <lux/engine/editor/EditorServiceRegistrar.hpp>
#include <lux/engine/editor/EditorUiRegistrar.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/SceneToolRegistrar.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::editor
{
    class EditorContext final
    {
    public:
        EditorContext(engine::EngineContext&, ProjectDescription);
        ~EditorContext() = default;
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;

        [[nodiscard]] engine::EngineContext& engine() const noexcept
        {
            return engine_;
        }
        [[nodiscard]] const ProjectDescription& project() const noexcept
        {
            return project_;
        }
        [[nodiscard]] asset::AssetVfs& assets() noexcept
        {
            return assets_;
        }
        [[nodiscard]] EditorServiceRegistrar& services() noexcept
        {
            return services_;
        }
        [[nodiscard]] EditorUiRegistrar& ui() noexcept
        {
            return ui_;
        }
        [[nodiscard]] SceneToolRegistrar& sceneTools() noexcept
        {
            return scene_tools_;
        }
        void freeze() noexcept;

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> service() noexcept
        {
            if (!object::ObjectRuntime::instance().isCurrent())
            {
                return cxx::unexpected(
                    FrameworkFailure{EFrameworkError::WRONG_THREAD, "Project services require owner thread"}
                );
            }
            return services_.get<T>(*this);
        }

    private:
        engine::EngineContext& engine_;
        ProjectDescription project_;
        asset::AssetVfs assets_;
        EditorUiRegistrar ui_;
        SceneToolRegistrar scene_tools_;
        // Services die before factory captures, project paths and VFS providers.
        EditorServiceRegistrar services_;
    };
} // namespace lux::editor
