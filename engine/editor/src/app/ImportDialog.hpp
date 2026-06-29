#pragma once
// ============================================================================
//  ImportDialog — the "Import Options" modal for model imports (axis-fix /
//  uniform scale / animations). Owns ONLY the modal's state + rendering; the
//  actual import is the caller's job, run through onConfirm (deferred out of
//  the ImGui frame, like every scene-mutating editor action).
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

#include <lux/engine/editor/import/AssetImporter.hpp>   // ImportOptions

#include <filesystem>
#include <functional>
#include <utility>

namespace lux::editor
{
    class ImportDialog
    {
    public:
        /// Called when the user presses "Import" — the editor wires this to
        /// enqueue doImport(source, options) on the deferred-action queue.
        using ConfirmFn =
            std::function<void(const std::filesystem::path&, const ImportOptions&)>;

        void setOnConfirm(ConfirmFn fn) { on_confirm_ = std::move(fn); }

        /// Arm the modal for @p source (resets the options to @p defaults).
        void open(const std::filesystem::path& source,
                  const ImportOptions& defaults = {})
        {
            source_  = source;
            options_ = defaults;
            open_    = true;
        }

        /// Paint the modal — a no-op until open() has been called. Must run
        /// inside a live ImGui frame (the editor calls it from the menu-bar hook).
        void paint();

    private:
        bool                  open_{false};
        std::filesystem::path source_;
        ImportOptions         options_{};
        ConfirmFn             on_confirm_;
    };

} // namespace lux::editor
