#pragma once
// ============================================================================
//  ImportController — owns the editor's asset-import subsystem: the Import
//  Options modal (ImportDialog) plus the model/texture import operations. The
//  editor forwards importExternalAsset() here (menu, OS file-drop). All heavy
//  work is deferred out of the ImGui frame through the editor's action queue.
//
//  Holds a LuxEditor& for the dependencies the flow needs at call time — the
//  current project, the deferred-action queue, the asset manager / browser, and
//  the toast queue — all of which are editor-owned and (for the project) mutate
//  over the editor's lifetime, so a back-reference is the natural seam.
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include "app/ImportDialog.hpp"   // owned modal (also pulls in ImportOptions)

#include <lux/engine/editor/import/AssetImporter.hpp>   // ImportReport(owned completion value)
#include <lux/cxx/core/move_only_function.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace lux::editor
{
    class LuxEditor;

    // ── 导入的异步三段式(批H2;设计 §7.9-⑤)────────────────────────────
    //
    // 确认导入 → typed operation(拥有型快照) → CPU 池上
    // importExternalFileDetached(散管理器物化 + 写盘,零活账本触碰)→
    // MainThreadScheduler 回主线程 → registerImportedFiles(账本注册,Shells 惰性
    // 路径)+ toast + 精确 EditorAssetChanged 事实。长导入不再冻结编辑器。
    // 判重从「活账本 id 碰撞」改为主线程**盘上判重**(manifest/产物在即已
    // 导入)—— 恰合「崩溃留下无 manifest 目录可重导」的既有恢复设计。
    // Progress 流是后续增量(SerDeser 无进度回调面,记档 §1.9 邻项)。

    struct ImportJob
    {
        std::filesystem::path source;
        std::filesystem::path dest_root;
        ImportOptions         options;
    };

    class ImportController
    {
    public:
        using ImportDispatch = lux::cxx::move_only_function<bool(
            std::uint64_t,
            std::shared_ptr<const ImportJob>)>;

        explicit ImportController(LuxEditor& editor);

        void setImportDispatch(ImportDispatch dispatch)
        {
            import_dispatch_ = std::move(dispatch);
        }

        /// MainThreadScheduler completion target. The committed content change is
        /// the only part broadcast to other editor consumers.
        void adoptImportResult(
            std::shared_ptr<ImportReport> report,
            const std::filesystem::path& source);

        /// Menu / OS-drop / programmatic entry point. Models open the Import
        /// Options modal (axis / scale / animations); textures import straight
        /// away with defaults. No-op (with a toast) when no project is open or
        /// the source is missing. Safe only from the main thread / a drained
        /// pending action — never from a GLFW callback or a scene-swapping frame.
        void importExternalAsset(const std::filesystem::path& source);

        /// Paint the Import Options modal — a no-op until a model import has
        /// armed it. Called from the menu-bar hook each frame (live ImGui frame).
        void paintDialog() { dialog_.paint(); }

    private:
        /// Run the actual import with the given options, refresh the AssetBrowser,
        /// and toast the outcome. Runs from the deferred queue (models, after the
        /// modal) or inline (textures).
        void doImport(const std::filesystem::path& source,
                      const ImportOptions& options);

        LuxEditor&    editor_;
        ImportDialog  dialog_;
        ImportDispatch import_dispatch_;
        std::uint64_t next_import_id_{ 0 };   ///< 批H2:请求配对(latest 无关,逐个完成)
    };

} // namespace lux::editor
