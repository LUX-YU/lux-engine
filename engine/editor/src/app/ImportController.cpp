#include "app/ImportController.hpp"

#include <lux/engine/editor/app/LuxEditor.hpp>          // editor accessors (project / queue / assets / toasts)
#include <lux/engine/editor/import/AssetImporter.hpp>   // isModelExtension / importExternalFile / ImportResult
#include <lux/engine/editor/panels/AssetBrowser.hpp>    // rescan()
#include <lux/engine/editor/panels/ToastQueue.hpp>      // ToastQueue::push / ToastLevel
#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>            // importExternalFile arg
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>

#include <lux/cxx/core/Format.hpp>
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
                lux::format("Import failed: '{}' not found.", source.string()),
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

        // 盘上判重(批H2):池上散管理器没有活账本的确定性 id 碰撞闸,判重
        // 提前到派发时看**产物** —— models 看 .luxmodel manifest(manifest
        // 最后写,恰合「崩溃留下无 manifest 目录可重导」的既有恢复设计),
        // textures 看 Textures/<stem>.luxasset。真重导入仍是记档项 §1.9。
        const auto stem = source.stem().string();
        bool already = false;
        std::error_code ec;
        if (isModelExtension(source.extension().string()))
            already = std::filesystem::exists(
                project->contentRoot() / "Models" / stem / (stem + ".luxmodel"), ec);
        else
            already = std::filesystem::exists(
                project->contentRoot() / "Textures" / (stem + ".luxasset"), ec);
        if (already)
        {
            editor_.toasts().push(
                lux::format("'{}' is already imported — re-import of edited "
                            "sources is not yet supported (§1.9).",
                            source.filename().string()),
                ToastLevel::Warning);
            return;
        }

        // 发布即返回：解析/转码/写盘在 CPU pool（散管理器，零活账本
        // 触碰）；结果由 MainThreadScheduler 交回本 controller 提交账本与 UI。
        auto job       = std::make_shared<const ImportJob>(
            ImportJob{ source, project->contentRoot(), options });
        const auto id  = ++next_import_id_;
        editor_.toasts().push(
            lux::format("Importing '{}'…", source.filename().string()),
            ToastLevel::Info);
        if (!import_dispatch_ || !import_dispatch_(id, std::move(job)))
        {
            editor_.toasts().push(
                "Import queue is stopping or full.",
                ToastLevel::Error);
        }
    }

    void ImportController::adoptImportResult(
        std::shared_ptr<ImportReport> report_owner,
        const std::filesystem::path& source)
    {
        if (!report_owner) return;
        const auto& report = *report_owner;

        // 主线程注册产物(账本线程契约):Shells 惰性路径,工程打开同款。
        if (!report.written.empty())
            (void)registerImportedFiles(report.written,
                                        editor_.assetManagerShared());

        // Publish one committed fact per successfully written asset. Partial
        // imports remain visible without collapsing their identity into a
        // project-wide invalidation.
        for (const auto& path : report.written)
        {
            const auto probe = lux::asset::readAssetHeader(path);
            if (probe.id.is_nil()) continue;
            editor_.events().publish(EditorAssetChanged{
                probe.id,
                EEditorAssetChange::ADDED,
                editor_.assetManagerShared()->contentRevision(probe.id),
                path
            });
        }

        if (report.result == ImportResult::OK)
        {
            editor_.toasts().push(
                lux::format("Imported '{}'  ({} file(s))",
                            source.filename().string(), report.written.size()),
                ToastLevel::Success);
        }
        else
        {
            editor_.toasts().push(
                lux::format("Import failed: {}", report.message),
                ToastLevel::Error);
        }
    }

} // namespace lux::editor
