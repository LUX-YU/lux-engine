#pragma once
namespace lux::project
{
    struct SceneRegistrations;
}

#include <lux/engine/editor/PaneManager.hpp>
#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/editor/metadata/CommandRegistration.hpp>
#include <filesystem>
#include <memory>
#include <span>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/TaskInfo.hpp>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::engine
{
    class EngineContext;
}
namespace lux::render
{
    class RenderRuntime;
}
namespace lux::scene
{
    class RenderResources;
}
namespace lux::project
{
    class PluginManager;
}
namespace lux::editor::assets
{
    class AssetImporter;
}

namespace lux::editor
{
    class ProjectStorage;
    class ComponentEditorRegistry;
    struct ConfigurationEditorRegistration;
    namespace detail
    {
        struct EditorContextAccess;
    }

    namespace tasks
    {
        class TaskMonitor;
    }

    // One project's shared facilities. Tool models and UI remain with their owners.
    class EditorContext final : public object::LuxObject
    {
    public:
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;
        ~EditorContext() override;

        [[nodiscard]] tasks::TaskMonitor& taskMonitor() noexcept;

        process::ExecutionRuntime& execution() noexcept;
        engine::EngineContext& engine() noexcept;
        ProjectStorage& project() noexcept;
        assets::AssetImporter& assetImporter() noexcept;
        render::RenderRuntime& renderRuntime() noexcept;
        lux::scene::RenderResources& renderResources() noexcept;
        const lux::project::PluginManager& plugins() const noexcept;
        const lux::project::SceneRegistrations& sceneRegistrations() const noexcept;
        const ComponentEditorRegistry& componentEditors() const noexcept;
        std::span<const ConfigurationEditorRegistration> configurationEditors() const noexcept;
        PaneManager& panes() noexcept;
        [[nodiscard]] EditorResult<void> setAssetEditors(std::vector<AssetEditorRegistration>);
        [[nodiscard]] std::span<const AssetEditorRegistration> assetEditors() const noexcept;
        [[nodiscard]] PaneRegistration::CreateResult openAsset(asset::AssetId) noexcept;
        [[nodiscard]] const std::filesystem::path& installation() const noexcept;
        [[nodiscard]] EditorResult<void> setCommands(std::vector<CommandRegistration>);
        [[nodiscard]] std::span<const CommandRegistration> commands() const noexcept;
        [[nodiscard]] std::uint64_t commandRevision() const noexcept;

    private:
        friend struct detail::EditorContextAccess;
        struct Impl;
        explicit EditorContext(lux::ui::Root&, std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
        PaneManager panes_;
    };
}
