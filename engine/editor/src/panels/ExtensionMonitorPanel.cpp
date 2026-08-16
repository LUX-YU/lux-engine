#include <lux/engine/editor/panels/ExtensionMonitorPanel.hpp>

#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>
#include <lux/engine/runtime/extensions/RenderEffects.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>

#include <imgui.h>

namespace lux::editor
{
    namespace
    {
        [[nodiscard]] const char* stateName(
            lux::extensions::EExtensionModuleState state) noexcept
        {
            using State = lux::extensions::EExtensionModuleState;
            switch (state)
            {
            case State::LOADED: return "loaded";
            case State::REGISTERING: return "registering";
            case State::READY: return "ready";
            case State::FAILED: return "failed";
            }
            return "unknown";
        }
    }

    ExtensionMonitorPanel::ExtensionMonitorPanel(
        std::string title,
        lux::extensions::EngineExtensions& extensions,
        const lux::runtime::SceneContributionCatalog& scene_contributions,
        const lux::runtime::RenderEffectCatalog& render_effects,
        const EditorPanelCatalog& editor_panels)
        : Panel(std::move(title), {620.f, 460.f})
        , extensions_(&extensions)
        , scene_contributions_(&scene_contributions)
        , render_effects_(&render_effects)
        , editor_panels_(&editor_panels)
    {}

    void ExtensionMonitorPanel::paint()
    {
        const auto snapshot = extensions_->runtimeSnapshot();
        ImGui::Text(
            "Admission: %s   queued: %zu   accounted: %zu bytes",
            snapshot.admission_open ? "open" : "closed",
            snapshot.queued_commands,
            snapshot.accounted_bytes);
        if (snapshot.active_operation)
        {
            ImGui::Text(
                "Active request generation: %llu   phase: %u",
                static_cast<unsigned long long>(
                    snapshot.active_operation->generation),
                static_cast<unsigned>(
                    snapshot.active_operation->phase));
        }

        if (ImGui::CollapsingHeader(
                "Modules", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const auto& module : snapshot.modules)
            {
                const auto module_name = module.id.name();
                ImGui::PushID(
                    module_name.data(),
                    module_name.data() + module_name.size());
                const bool open = ImGui::TreeNode(
                    "module",
                    "%.*s  %u.%u.%u  [%s]",
                    static_cast<int>(module_name.size()),
                    module_name.data(),
                    module.version.major,
                    module.version.minor,
                    module.version.patch,
                    stateName(module.state));
                if (open)
                {
                    if (module.origin.kind ==
                        lux::extensions::EExtensionModuleSource::FILE_PATH)
                    {
                        ImGui::TextWrapped(
                            "binary: %s",
                            module.origin.path.string().c_str());
                    }
                    else
                    {
                        ImGui::TextWrapped(
                            "binary: memory:%s (%zu bytes)",
                            module.origin.hint.c_str(),
                            module.origin.image_bytes);
                    }
                    for (const auto& dependency : module.dependencies)
                    {
                        ImGui::BulletText(
                            "%.*s  major=%u minor>=%u",
                            static_cast<int>(dependency.id.name().size()),
                            dependency.id.name().data(),
                            dependency.required_major,
                            dependency.minimum_minor);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Contributions"))
        {
            ImGui::Text("Scene contributions: %zu", scene_contributions_->all().size());
            for (const auto& descriptor : scene_contributions_->all())
                ImGui::BulletText(
                    "%.*s",
                    static_cast<int>(descriptor.id.name().size()),
                    descriptor.id.name().data());

            ImGui::Text("Render effects: %zu", render_effects_->all().size());
            for (const auto& descriptor : render_effects_->all())
                ImGui::BulletText(
                    "%.*s",
                    static_cast<int>(descriptor.id.name().size()),
                    descriptor.id.name().data());

            ImGui::Text("Editor panels: %zu", editor_panels_->all().size());
            for (const auto& descriptor : editor_panels_->all())
                ImGui::BulletText(
                    "%.*s",
                    static_cast<int>(descriptor.id.name().size()),
                    descriptor.id.name().data());
        }
    }
}
