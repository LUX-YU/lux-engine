#pragma once
/**
 * @file ProjectPickerPanel.hpp
 * @brief The launcher's main panel — Recent list + New / Open buttons.
 *
 * Paints a single full-window ImGui pane:
 *
 *   ┌───────────────────────────────────────────────────┐
 *   │ Lux Launcher                                       │
 *   ├───────────────────────────────────────────────────┤
 *   │ Recent Projects:                                   │
 *   │   ▸ C:/.../EmptyProject/MyGame.luxproject          │
 *   │   ▸ E:/.../OtherProject/Other.luxproject           │
 *   │   ...                                              │
 *   │                                                    │
 *   │ [ New Project... ]  [ Open Project... ]            │
 *   └───────────────────────────────────────────────────┘
 *
 * Clicking a recent → fires the on-chosen callback with that path.
 * Open Project → modal path input, then on-chosen callback.
 * New Project → modal with root-directory input, runs
 *   `lux::project::Project::newOnDisk` then fires on-chosen.
 *
 * The panel does NOT spawn the editor itself — that's the LauncherApp's
 * job. Decoupling lets the panel be unit-tested in isolation (future).
 */

#include <lux/engine/launcher/visibility.h>

#include <lux/engine/ui/Panel.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lux::launcher
{
    class LUX_ENGINE_LAUNCHER_PUBLIC ProjectPickerPanel
        : public lux::ui::Panel
    {
    public:
        using OnChosen = std::function<void(const std::filesystem::path&)>;

        explicit ProjectPickerPanel(OnChosen on_chosen);
        ~ProjectPickerPanel() override = default;

    private:
        // Mode of the modal popup the panel may currently be hosting.
        enum class Modal { None, OpenProject, NewProject, NewDemoProject };

        void paint() override;

        // Paint the path-input popup that drives Open and New.
        void paintModal();

        // Fire user-supplied callback. The launcher writes the recent-
        // projects file and spawns the editor.
        void emit(const std::filesystem::path& p);

        OnChosen                               on_chosen_;

        // Cached list of recents — re-read on every paint() so an
        // editor-side push lands without restarting the launcher.
        // Cheap (filesystem read of one small file).
        std::vector<std::filesystem::path>     recents_cache_;

        // Modal state.
        Modal                                  modal_{Modal::None};
        bool                                   modal_open_requested_{false};
        std::array<char, 1024>                 path_buffer_{};
        std::string                            modal_error_;
    };

} // namespace lux::launcher
