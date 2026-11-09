#pragma once
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>

namespace fixture
{
    template <class Assembly>
    auto createContext(
        lux::engine::Engine& engine,
        lux::editor::ProjectDescription description,
        lux::editor::ProjectManifest manifest,
        Assembly&& assembly
    ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::editor::EditorContext>>
    {
        using namespace lux;
        using namespace lux::editor;
        if (auto registered = registerProjectErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerContextErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        EditorComposition composition;
        if (auto assembled = assembly(composition); !assembled)
        {
            return cxx::unexpected(assembled.error());
        }
        auto plugins = plugins::PluginManager::create({}, {});
        if (!plugins)
        {
            return cxx::unexpected(error::Error{Errors::ProjectPlugins});
        }
        auto registrations = plugins::readSceneRegistrations();
        if (!registrations)
        {
            return cxx::unexpected(error::Error{Errors::ProjectPlugins});
        }
        description.manifest_file = description.root / "Fixture.luxproj";
        return detail::createEditorContext(
            engine,
            {std::move(description),
             std::move(manifest),
             std::make_unique<plugins::PluginManager>(std::move(*plugins)),
             std::make_shared<const plugins::SceneRegistrations>(std::move(*registrations))},
            std::move(composition)
        );
    }
} // namespace fixture
