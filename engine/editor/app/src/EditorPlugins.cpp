#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <imgui.h>

#include <algorithm>

namespace lux::editor
{
namespace
{
EditorFailure pluginFailure(const PluginFailure &failure)
{
    return {EEditorError::SOURCE_FAILURE, "plugin." + failure.plugin,
            static_cast<std::uint64_t>(failure.code), failure.subject + ": " + failure.detail, failure};
}

std::string failureText(const EditorFailure &failure)
{
    return failure.domain + ":" + std::to_string(failure.reason) + " " + failure.message;
}
}

Editor::PluginPane::PluginPane(Editor &editor)
    : Object(editor.messages_.dispatcherRef(), lux::ui::PaneId{"plugins"},
             lux::ui::PaneTypeId{"lux.editor.plugins"}, "Plugins"), editor_(editor)
{
}

void Editor::PluginPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &)
{
    auto &data = *editor_.impl_;
    frame.textWrapped(data.plugin_status);
    ImGui::InputText("Description file", description_.data(), description_.size());
    ImGui::InputText("Installation root", root_.data(), root_.size());
    ImGui::BeginDisabled(data.plugin_request.has_value() || editor_.closing());
    if (frame.smallButton("Read description"))
    {
        const auto result = editor_.readPluginDescription(std::filesystem::u8path(description_.data()),
                                                         std::filesystem::u8path(root_.data()));
        if (!result) data.plugin_status = failureText(result.error());
    }
    for (const auto &plugin : data.metadata.plugins())
    {
        ImGui::PushID(plugin.identity.id.c_str());
        const bool loaded = std::ranges::any_of(data.plugins, [&](const auto &value) {
            return value->identity() == plugin.identity;
        });
        frame.textWrapped(plugin.identity.id + (loaded ? " — available" : " — not loaded"));
        frame.textWrapped(plugin.description);
        for (const auto &ability : plugin.abilities)
        {
            frame.textWrapped(ability.display_name + " [" + ability.identity.id + "]");
            for (const auto *implementation : data.metadata.implementations(ability.identity.id))
                ImGui::BulletText("%s", implementation->system.c_str());
        }
        if (loaded)
        {
            const auto library = std::ranges::find_if(data.plugins, [&](const auto &value) {
                return value->identity() == plugin.identity;
            });
            if (const auto *tools = (*library)->editorExports())
            {
                for (std::uint32_t index{}; index < tools->configuration_count; ++index)
                {
                    const auto &record = tools->configurations[index];
                    if (frame.smallButton(std::string("Inspect ") + record.schema_name))
                    {
                        auto value = ConfigurationValue::create(record, (*library)->editorCode());
                        if (value) configuration_ = std::move(*value);
                        else data.plugin_status = failureText(pluginFailure(value.error()));
                    }
                }
            }
        }
        if (!loaded && frame.smallButton("Load implementations"))
        {
            const auto result = editor_.loadPlugin(plugin.identity.id);
            if (!result) data.plugin_status = failureText(result.error());
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (configuration_)
    {
        frame.textMuted("Configuration preview; project selections remain in the document.");
        static_cast<void>(configuration_->edit(frame));
    }
    if (data.plugin_request && frame.smallButton("Cancel loading")) editor_.cancelPluginLoad();
}

EditorResult<void> Editor::readPluginDescription(std::filesystem::path file, std::filesystem::path root)
{
    if (!impl_ || closing() || impl_->plugin_request)
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugin.read"});
    auto scheduler = impl_->process.blocking();
    if (!scheduler)
        return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "plugin.read", 0, {}, scheduler.error()});
    Impl::PluginWork work{[file = std::move(file), root = std::move(root)]() mutable
        -> EditorResult<Impl::PluginPrepared> {
        EngineMetadata metadata;
        auto result = metadata.read(file, root);
        if (!result) return lux::cxx::unexpected(pluginFailure(result.error()));
        return Impl::PluginPrepared{std::move(metadata), {}};
    }};
    auto &request = impl_->plugin_request.emplace();
    request.work = std::make_unique<Impl::PluginTask>(impl_->process,
        stdexec::then(stdexec::schedule(*scheduler), std::move(work)));
    impl_->plugin_status = "Reading description...";
    request.work->start();
    return {};
}

EditorResult<void> Editor::loadPlugin(std::string id)
{
    if (!impl_ || closing() || impl_->plugin_request)
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugin.load"});
    auto order = impl_->metadata.loadOrder(id);
    if (!order) return lux::cxx::unexpected(pluginFailure(order.error()));
    std::vector<PluginDescription> descriptions;
    for (const auto *description : *order)
    {
        const bool loaded = std::ranges::any_of(impl_->plugins, [&](const auto &value) {
            return value->identity() == description->identity;
        });
        if (!loaded) descriptions.push_back(*description);
    }
    if (descriptions.empty())
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "plugin.already_loaded"});
    auto scheduler = impl_->process.blocking();
    if (!scheduler)
        return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE, "plugin.load", 0, {}, scheduler.error()});
    Impl::PluginWork work{[descriptions = std::move(descriptions), dependencies = impl_->plugins]() mutable
        -> EditorResult<Impl::PluginPrepared> {
        Impl::PluginPrepared result;
        for (const auto &description : descriptions)
        {
            auto loaded = PluginLibrary::load(description, dependencies);
            if (!loaded) return lux::cxx::unexpected(pluginFailure(loaded.error()));
            dependencies.push_back(*loaded);
            result.libraries.push_back(std::move(*loaded));
        }
        return result;
    }};
    auto &request = impl_->plugin_request.emplace();
    request.id = std::move(id);
    request.work = std::make_unique<Impl::PluginTask>(impl_->process,
        stdexec::then(stdexec::schedule(*scheduler), std::move(work)));
    impl_->plugin_status = "Loading " + request.id;
    request.work->start();
    return {};
}

void Editor::cancelPluginLoad() noexcept
{
    if (!impl_ || !impl_->plugin_request) return;
    auto &request = *impl_->plugin_request;
    request.cancelled = true;
    if (!request.work && impl_->renderer) static_cast<void>(impl_->renderer->cancelFeatureRegistration());
}

void Editor::advancePlugins()
{
    if (!impl_ || !impl_->plugin_request) return;
    auto &data = *impl_;
    auto &request = *data.plugin_request;
    const auto failed = [&](const EditorFailure &failure) {
        data.plugin_status = failureText(failure);
        data.plugin_request.reset();
    };
    if (request.work)
    {
        if (!request.work->ready()) return;
        auto result = request.work->take();
        request.work.reset(); // Main delivery has returned; its captured code can now be released.
        if (request.cancelled)
        {
            data.plugin_status = "Loading cancelled";
            data.plugin_request.reset();
            return;
        }
        if (!result)
        {
            failed(result.error());
            return;
        }
        if (result->metadata)
        {
            const auto merged = data.metadata.append(std::move(*result->metadata));
            if (!merged)
            {
                failed(pluginFailure(merged.error()));
                return;
            }
            data.plugin_status = "Description registered for this session";
            data.plugin_request.reset();
            return;
        }
        request.libraries = std::move(result->libraries);
        std::vector<render::RenderFeatureRegistration> features;
        for (const auto &library : request.libraries)
        {
            if (const auto *tools = library->editorExports())
            {
                if (!request.reflection) request.reflection.emplace(meta::ReflectionRegistry::beginDraft());
                const auto appended = request.reflection->append(tools->register_types, library->editorCode());
                if (!appended)
                {
                    failed({EEditorError::SOURCE_FAILURE, "plugin.reflection", 0, {}, appended.error()});
                    return;
                }
                for (const auto &entry : std::span{tools->configurations, tools->configuration_count})
                {
                    const auto *type = entry.reflection(*request.reflection->registry());
                    const bool invalid_type = !type || type->type.ptr != type ||
                        type->type.hash != entry.codec.type.hash() || type->type.name != entry.codec.type.name();
                    if (invalid_type)
                    {
                        failed({EEditorError::SOURCE_FAILURE, "plugin.configuration", 0, entry.schema_name});
                        return;
                    }
                }
            }
            features.insert(features.end(), library->renderFeatures().begin(), library->renderFeatures().end());
        }
        // Construct immutable inputs for future documents before any backend admission.
        // Active documents keep their own registrations, instances and task graphs.
        auto plugins = data.plugins;
        plugins.insert(plugins.end(), request.libraries.begin(), request.libraries.end());
        request.documents = registrations_;
        for (const auto &provider : config_.providers)
        {
            auto document = provider.registration(data.process, *data.renderer, plugins);
            if (!document)
            {
                failed(document.error());
                return;
            }
            const auto found = std::ranges::find(request.documents, document->type, &DocumentRegistration::type);
            if (found == request.documents.end()) request.documents.push_back(std::move(*document));
            else *found = std::move(*document);
        }
        const auto admitted = data.renderer->beginFeatureRegistration(std::move(features));
        if (!admitted)
        {
            failed({EEditorError::FRONTEND_FAILURE, "plugin.render", 0, {}, admitted.error()});
            return;
        }
    }
    const auto status = data.renderer->featureRegistrationStatus();
    if (status.state == render::EFeatureRegistrationState::READY && !request.cancelled)
    {
        if (request.reflection)
        {
            const auto prepared = request.reflection->prepareCommit();
            if (!prepared)
            {
                data.plugin_status = "Reflection changed during plugin loading; registration rolled back";
                cancelPluginLoad();
                return;
            }
        }
        // A single Main turn publishes the prevalidated reflection, backend catalog,
        // document inputs and available-library list. No callbacks run between them.
        const auto adopted = data.renderer->commitFeatureRegistration();
        if (!adopted)
        {
            data.plugin_status = "Render registration could not be adopted";
            cancelPluginLoad();
            return;
        }
        if (request.reflection && !request.reflection->commit()) std::terminate();
        registrations_ = std::move(request.documents);
        data.plugins.insert(data.plugins.end(), request.libraries.begin(), request.libraries.end());
        data.plugin_status = request.id + " is available for new instances";
        data.plugin_request.reset();
    }
    else if (status.state == render::EFeatureRegistrationState::FAILED ||
             status.state == render::EFeatureRegistrationState::CANCELLED)
    {
        if (status.state == render::EFeatureRegistrationState::FAILED)
            data.plugin_status = "Render registration failed: " + std::to_string(status.error.type.index);
        else data.plugin_status = "Loading cancelled; accepted registrations have been rolled back";
        data.plugin_request.reset();
    }
}
} // namespace lux::editor
