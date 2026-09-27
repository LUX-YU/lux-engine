#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/detail/EditorContextStartup.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
#include <lux/engine/editor/metadata/EditorPlugin.hpp>
#include <lux/engine/editor/metadata/EditorReflection.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/scene/RenderResources.hpp>

#include <algorithm>
#include <thread>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        EditorFailure pluginFailure(const project::PluginFailure& error)
        {
            return {
                EEditorError::SOURCE_FAILURE,
                "project.plugins",
                static_cast<std::uint64_t>(error.code),
                error.plugin + ": " + error.subject + ": " + error.detail,
                error
            };
        }

        using PreparedPlugins = std::pair<project::PluginManager, std::vector<EditorPlugin>>;
        EditorResult<PreparedPlugins> preparePlugins(
            const std::filesystem::path& project_root,
            std::span<const ProjectPluginEntry> selection,
            const std::filesystem::path& installation
        ) noexcept
        try
        {
            project::PluginCatalog catalog;
            if (!installation.empty())
            {
                auto read = catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation);
                if (!read)
                    return lux::cxx::unexpected(pluginFailure(read.error()));
            }
            std::vector<project::MetadataIdentity> selected;
            std::vector<std::string> descriptions;
            for (const auto& plugin : selection)
            {
                selected.push_back({plugin.id, plugin.version});
                const bool is_missing_description = plugin.description_path.empty();
                const bool is_duplicate_description =
                    std::ranges::find(descriptions, plugin.description_path) != descriptions.end();
                const bool should_skip_description = is_missing_description || is_duplicate_description;
                if (should_skip_description)
                    continue;
                std::error_code error;
                const auto path = std::filesystem::canonical(project_root / plugin.description_path, error);
                const auto relative = error ? std::filesystem::path{} : path.lexically_relative(project_root);
                const bool has_path_error = static_cast<bool>(error);
                const bool is_empty_relative_path = relative.empty();
                const bool is_absolute_relative_path = relative.is_absolute();
                const bool has_parent_traversal =
                    !has_path_error && !is_empty_relative_path && !is_absolute_relative_path &&
                    std::ranges::any_of(relative, [](const auto& part) { return part == ".."; });
                const bool escapes =
                    has_path_error || is_empty_relative_path || is_absolute_relative_path || has_parent_traversal;
                if (escapes)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::SOURCE_FAILURE, "project.plugins.path", 0, plugin.description_path}
                    );
                auto read = catalog.read(path, project_root);
                if (!read)
                    return lux::cxx::unexpected(pluginFailure(read.error()));
                descriptions.push_back(plugin.description_path);
            }
            auto loaded = project::PluginManager::create(std::move(catalog), selected);
            if (!loaded)
                return lux::cxx::unexpected(pluginFailure(loaded.error()));
            std::vector<EditorPlugin> extensions;
            for (const auto& runtime : loaded->libraries())
            {
                const auto* description = loaded->catalog().find(runtime->identity().id);
                auto extension = loadEditorPlugin(*description, *runtime, extensions);
                if (!extension)
                    return lux::cxx::unexpected(pluginFailure(extension.error()));
                extensions.push_back(std::move(*extension));
            }
            return PreparedPlugins{std::move(*loaded), std::move(extensions)};
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.plugins"});
        }
    }

    struct EditorContext::Impl final
    {
        // Queue and code owners outlive every task, registry, resource and callback using them.
        engine::EngineContext& engine;
        object::ObjectMessageQueue& messages;
        process::ExecutionRuntime& execution;
        std::uint64_t task_revision{};
        std::shared_ptr<const void> reflection;
        std::optional<project::PluginManager> plugins;
        std::vector<EditorPlugin> editor_plugins;
        process::TaskScope project_tasks{execution};
        std::unique_ptr<ProjectStorage> project;
        SceneRegistrations registrations;
        ComponentEditorRegistry component_editors;
        std::vector<ConfigurationEditorRegistration> configuration_editors;
        std::filesystem::path installation;
        std::vector<AssetEditorRegistration> asset_editors;
        std::vector<CommandRegistration> commands;
        std::uint64_t command_revision{};
        engine::RenderContext& rendering;
        std::unique_ptr<assets::AssetImporter> importer;

        Impl(engine::EngineContext& application, object::ObjectMessageQueue& queue) noexcept
            : engine(application), messages(queue), execution(application.execution()),
              rendering(*application.renderContext())
        {}

        template <class Scheduler, class Work> auto prepare(Scheduler scheduler, Work work)
        {
            using Result = std::invoke_result_t<Work>;
            std::optional<Result> result;
            process::TaskScope tasks(execution);
            auto admitted = tasks.submit(
                {"Prepare editor context", "Startup"},
                [scheduler, work = std::move(work)](process::TaskReporter) mutable noexcept {
                    return stdexec::then(stdexec::schedule(scheduler), std::move(work));
                },
                [&result](auto&& completed) noexcept { result.emplace(detail::taskResult(std::move(completed))); }
            );
            if (!admitted)
                return Result{lux::cxx::unexpected(
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "context.prepare", 0, {}, admitted.error()}
                )};
            if (!execution.waitUntil([&]() noexcept { return result.has_value(); }))
                std::terminate();
            return std::move(*result);
        }

        EditorResult<void> initialize(EditorContext& context, detail::EditorContextCreateInfo& info)
        {
            const auto blocking = execution.blocking();
            if (!blocking)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "context.blocking"});
            auto source =
                prepare(*blocking, [file = info.project_file]() noexcept { return readProjectOpenData(file); });
            if (!source)
                return lux::cxx::unexpected(std::move(source.error()));
            auto opened =
                ProjectStorage::open(*source, engine.assets(), *blocking, project_tasks, messages.dispatcherRef());
            if (!opened)
                return lux::cxx::unexpected(std::move(opened.error()));
            project = std::move(*opened);
            auto loaded = prepare(
                *blocking,
                [root = project->root(), selection = project->manifest().plugins, installation = info.plugin_root](
                ) noexcept { return preparePlugins(root, selection, installation); }
            );
            if (!loaded)
                return lux::cxx::unexpected(std::move(loaded.error()));
            plugins.emplace(std::move(loaded->first));
            editor_plugins = std::move(loaded->second);
            reflection = acquireEditorReflection();
            auto draft = meta::ReflectionRegistry::beginDraft();
            auto editors = std::move(info.product_editors);
            installation = info.plugin_root;
            std::vector<PaneRegistration> panes;
            for (const auto& extension : editor_plugins)
            {
                if (!extension.exports)
                    continue;
                auto appended = draft.appendOnce(extension.exports->register_types, extension.code);
                if (!appended)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::SOURCE_FAILURE, "plugin.reflection", 0, {}, appended.error()}
                    );
                for (const auto& entry :
                     std::span{extension.exports->configurations, extension.exports->configuration_count})
                {
                    auto registered = entry;
                    registered.code_lifetime = extension.code;
                    configuration_editors.push_back(std::move(registered));
                    const auto* type = entry.reflection(*draft.registry());
                    const bool invalid = !type || type->type.ptr != type ||
                                         type->type.hash != entry.codec.type.hash() ||
                                         type->type.name != entry.codec.type.name();
                    if (invalid)
                        return lux::cxx::unexpected(
                            EditorFailure{EEditorError::SOURCE_FAILURE, "plugin.configuration", 0, entry.schema_name}
                        );
                }
                for (auto entry :
                     std::span{extension.exports->component_editors, extension.exports->component_editor_count})
                {
                    // The defining DLL must survive the return from the Element's virtual destructor.
                    entry.code_lifetime = extension.code;
                    editors.push_back(std::move(entry));
                }
                for (auto entry : std::span{extension.exports->panes, extension.exports->pane_count})
                {
                    entry.code_lifetime = extension.code;
                    panes.push_back(std::move(entry));
                }
                for (auto entry : std::span{extension.exports->commands, extension.exports->command_count})
                {
                    entry.code_lifetime = extension.code;
                    commands.push_back(std::move(entry));
                }
                for (auto entry : std::span{extension.exports->asset_editors, extension.exports->asset_editor_count})
                {
                    entry.code_lifetime = extension.code;
                    asset_editors.push_back(std::move(entry));
                }
            }
            auto registered_commands = context.setCommands(std::move(commands));
            if (!registered_commands)
                return registered_commands;
            auto registered_panes = context.panes().setRegistrations(std::move(panes));
            if (!registered_panes)
                return registered_panes;
            auto registered_assets = context.setAssetEditors(std::move(asset_editors));
            if (!registered_assets)
                return registered_assets;
            auto scene_types = lux::editor::sceneRegistrations({}, plugins->libraries());
            if (!scene_types)
                return lux::cxx::unexpected(pluginFailure(scene_types.error()));
            registrations = std::move(*scene_types);
            // Product UI factories are optional capabilities; selected Runtime schemas decide availability.
            std::erase_if(editors, [&](const auto& entry) {
                return entry.provider.id == "lux.editor.product" && !registrations.components.find(entry.type);
            });
            auto editor_types = ComponentEditorRegistry::create(registrations.components, std::move(editors));
            if (!editor_types)
                return lux::cxx::unexpected(editor_types.error());
            component_editors = std::move(*editor_types);
            const auto verified = draft.prepareCommit();
            if (!verified)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "plugin.reflection", 0, {}, verified.error()}
                );
            auto features = std::move(info.product_features);
            features.insert(features.end(), registrations.features.begin(), registrations.features.end());
            auto registered = rendering.registerFeatures(std::move(features));
            if (!registered)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::FRONTEND_FAILURE, "context.features", 0, {}, registered.error()}
                );
            importer = std::make_unique<assets::AssetImporter>(*project, execution);
            auto committed = draft.commit();
            if (!committed)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "plugin.reflection.commit", 0, {}, committed.error()}
                );
            return {};
        }
    };

    EditorContext::EditorContext(lux::ui::Root& root, std::unique_ptr<Impl> impl) noexcept
        : LuxObject(impl->messages.dispatcherRef()), impl_(std::move(impl)), panes_(root, *this)
    {
        impl_->execution.setTaskObserver(
            this,
            [](void* owner, std::span<const process::TaskId> ids, bool reset) noexcept {
                auto& context = *static_cast<EditorContext*>(owner);
                if (reset)
                {
                    ++context.impl_->task_revision;
                    detail::reportSignalDelivery(context.emit(context.tasksReset), "tasks.reset");
                }
                for (auto id : ids)
                {
                    ++context.impl_->task_revision;
                    detail::reportSignalDelivery(context.emit(context.taskChanged, id), "tasks.changed");
                }
            }
        );
    }
    EditorContext::~EditorContext()
    {
        impl_->execution.setTaskObserver(nullptr, nullptr);
    }
    std::uint64_t EditorContext::taskRevision() const noexcept
    {
        return impl_->task_revision;
    }
    process::ExecutionRuntime& EditorContext::execution() noexcept
    {
        return impl_->execution;
    }
    engine::EngineContext& EditorContext::engine() noexcept
    {
        return impl_->engine;
    }
    ProjectStorage& EditorContext::project() noexcept
    {
        return *impl_->project;
    }
    assets::AssetImporter& EditorContext::assetImporter() noexcept
    {
        return *impl_->importer;
    }
    render::RenderRuntime& EditorContext::renderRuntime() noexcept
    {
        return impl_->rendering.runtime();
    }
    lux::scene::RenderResources& EditorContext::renderResources() noexcept
    {
        return impl_->rendering.resources();
    }
    const project::PluginManager& EditorContext::plugins() const noexcept
    {
        return *impl_->plugins;
    }
    const SceneRegistrations& EditorContext::sceneRegistrations() const noexcept
    {
        return impl_->registrations;
    }
    const ComponentEditorRegistry& EditorContext::componentEditors() const noexcept
    {
        return impl_->component_editors;
    }

    EditorResult<void> detail::EditorContextAccess::create(
        std::unique_ptr<EditorContext>& context,
        lux::ui::Root& root,
        engine::EngineContext& engine,
        object::ObjectMessageQueue& messages,
        EditorContextCreateInfo info
    ) noexcept
    {
        const bool has_context = static_cast<bool>(context);
        const bool is_missing_render_runtime = !engine.renderContext();
        const bool is_invalid_context = has_context || is_missing_render_runtime;
        if (is_invalid_context)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "context.create"});
        try
        {
            auto data = std::make_unique<EditorContext::Impl>(engine, messages);
            context = std::unique_ptr<EditorContext>(new EditorContext(root, std::move(data)));
            return context->impl_->initialize(*context, info);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "context.create"});
        }
    }

}

namespace lux::editor
{
    std::span<const ConfigurationEditorRegistration> EditorContext::configurationEditors() const noexcept
    {
        return impl_->configuration_editors;
    }
    PaneManager& EditorContext::panes() noexcept
    {
        return panes_;
    }
    const std::filesystem::path& EditorContext::installation() const noexcept
    {
        return impl_->installation;
    }
    std::span<const AssetEditorRegistration> EditorContext::assetEditors() const noexcept
    {
        return impl_->asset_editors;
    }
    EditorResult<void> EditorContext::setAssetEditors(std::vector<AssetEditorRegistration> registrations)
    {
        if (panes_.frozen())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "assets.registration"});
        std::unordered_set<std::string_view> names;
        for (const auto& entry : registrations)
        {
            const bool is_invalid = !entry.valid();
            const bool is_duplicate = !is_invalid && !names.insert(entry.type.name()).second;
            const bool has_factory = !is_invalid && std::ranges::any_of(panes_.registrations(), [&](const auto& pane) {
                return pane.type == entry.type;
            });
            if (is_invalid || is_duplicate || !has_factory)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "assets.registration"});
        }
        impl_->asset_editors = std::move(registrations);
        return {};
    }
    PaneRegistration::CreateResult EditorContext::openAsset(asset::AssetId id) noexcept
    {
        if (panes_.frozen())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "asset.activation"});
        const auto* asset = project().asset(id);
        if (!asset)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "asset.activation"});
        const AssetEditorRegistration* selected{};
        for (const auto& entry : impl_->asset_editors)
        {
            if (!entry.accepts(*asset))
                continue;
            if (selected)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "asset.ambiguous"});
            selected = &entry;
        }
        if (!selected)
            return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "asset.activation"});
        auto result = selected->open(panes_, id);
        if (result)
            panes_.show(result->get());
        return result;
    }
    EditorResult<void> EditorContext::setCommands(std::vector<CommandRegistration> commands)
    {
        if (panes_.frozen())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "commands.registration"});
        std::unordered_set<std::string_view> names;
        names.reserve(commands.size());
        for (const auto& entry : commands)
        {
            const bool is_invalid = !entry.valid();
            const bool is_duplicate = !is_invalid && !names.insert(entry.id.name()).second;
            if (is_invalid || is_duplicate)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "commands.registration"});
        }
        // Swap destroys old callables before their corresponding code pins, including replacement.
        impl_->commands.swap(commands);
        ++impl_->command_revision;
        return {};
    }
    std::span<const CommandRegistration> EditorContext::commands() const noexcept
    {
        return impl_->commands;
    }
    std::uint64_t EditorContext::commandRevision() const noexcept
    {
        return impl_->command_revision;
    }

}
