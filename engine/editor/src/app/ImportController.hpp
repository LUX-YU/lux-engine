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

#include <filesystem>

namespace lux::editor
{
    class LuxEditor;

    class ImportController
    {
    public:
        explicit ImportController(LuxEditor& editor);

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

        LuxEditor&   editor_;
        ImportDialog dialog_;
    };

} // namespace lux::editor
