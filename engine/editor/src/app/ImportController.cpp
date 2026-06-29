#include "app/ImportController.hpp"

#include <lux/engine/editor/app/LuxEditor.hpp>          // editor accessors (project / queue / assets / toasts)
#include <lux/engine/editor/import/AssetImporter.hpp>   // isModelExtension / importExternalFile / ImportResult
#include <lux/engine/editor/panels/AssetBrowser.hpp>    // rescan()
#include <lux/engine/editor/panels/ToastQueue.hpp>      // ToastQueue::push / ToastLevel
#include <lux/engine/editor/project/Project.hpp>        // contentRoot()
#include <lux/engine/asset/AssetManager.hpp>            // importExternalFile arg

#include <format>
#include <string>

namespace lux::editor
{
    ImportController::ImportController(LuxEditor& editor)
        : editor_(editor)
    {
        // On "Import", defer the heavy import out of the ImGui frame through the
        // editor's action queue (same rule as the menu / drop paths).
        dialog_.setOnConfirm(
            [this](const std::filesystem::path& src, const ImportOptions& opts)
            {
                editor_.enqueue([this, src, opts]{ doImport(src, opts); });
            });
    }

    void ImportController::importExternalAsset(const std::filesystem::path& source)
    {
        if (!editor_.currentProject())
        {
            editor_.toasts().push("Open a project before importing assets.",
                                  ToastLevel::Warning);
            return;
        }
        if (source.empty() || !std::filesystem::exists(source))
        {
            editor_.toasts().push(
                std::format("Import failed: '{}' not found.", source.string()),
                ToastLevel::Error);
            return;
        }

        // Models open the Import Options dialog (axis / scale / animations);
        // other assets (textures) import straight away with defaults. The modal
        // resets to defaults on each open(); on confirm it defers doImport.
        if (isModelExtension(source.extension().string()))
            dialog_.open(source);
        else
            doImport(source, {});
    }

    void ImportController::doImport(const std::filesystem::path& source,
                                    const ImportOptions& options)
    {
        // The project may have been closed between opening the dialog and
        // pressing Import (the action is deferred through the editor's queue).
        auto* project = editor_.currentProject();
        if (!project)
        {
            editor_.toasts().push("Import cancelled: no project open.",
                                  ToastLevel::Warning);
            return;
        }

        const auto report = importExternalFile(
            source, project->contentRoot(), editor_.assetManagerShared(), options);

        // Surface the new files immediately regardless of outcome — a partial
        // import may still have written some sub-assets. One broadcast refreshes
        // BOTH the browser (filesystem walk) and the registry (picker index);
        // this used to only rescan the browser, leaving new textures missing from
        // the inspector / material-graph pickers until some later refresh.
        editor_.events().content_changed.emit({});

        if (report.result == ImportResult::OK)
        {
            editor_.toasts().push(
                std::format("Imported '{}'  ({} file(s))",
                            source.filename().string(), report.written.size()),
                ToastLevel::Success);
        }
        else
        {
            const bool already =
                report.result == ImportResult::ImportFailed &&
                report.message.find("already imported") != std::string::npos;
            editor_.toasts().push(
                std::format("Import failed: {}", report.message),
                already ? ToastLevel::Warning : ToastLevel::Error);
        }
    }

} // namespace lux::editor
