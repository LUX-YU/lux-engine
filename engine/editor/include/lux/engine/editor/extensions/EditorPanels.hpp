#pragma once
/**
 * @file EditorPanels.hpp
 * @brief Main-thread ownership of Editor panels and UISystem registrations.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/PanelId.hpp>
#include <lux/engine/editor/visibility.h>
#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/ui/UISystem.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lux::editor
{
    struct EditorPanelSpec final
    {
        PanelId id;
        std::string display_name;
        bool default_visible{true};
        bool supports_removal{true};
        lux::extensions::ExtensionId provider;
    };

    enum class EEditorPanelInstallError : std::uint8_t
    {
        INVALID_PANEL,
        DUPLICATE_PANEL,
        ID_COLLISION,
        UI_REGISTRATION_FAILED,
        MODULE_UNAVAILABLE
    };

    struct EditorPanelSnapshot final
    {
        PanelId panel;
        std::string display_name;
        bool default_visible{true};
        bool visible{false};
        lux::extensions::ExtensionId provider;
    };

    class EditorPanelInstallContext;

    /// A thin main-thread owner. UISystem remains the sole registration
    /// mechanism; this class only keeps the Panel, its RAII registration and
    /// the optional provider lease in the required destruction order.
    class LUX_EDITOR_PUBLIC EditorPanels final
    {
    public:
        explicit EditorPanels(lux::ui::UISystem& ui);
        ~EditorPanels();
        EditorPanels(const EditorPanels&) = delete;
        EditorPanels& operator=(const EditorPanels&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EEditorPanelInstallError> add(
            EditorPanelSpec spec,
            std::unique_ptr<lux::ui::Panel> panel);
        [[nodiscard]] bool setVisible(
            PanelIdView id,
            bool visible) noexcept;
        [[nodiscard]] bool remove(PanelIdView id) noexcept;
        [[nodiscard]] lux::ui::Panel* find(PanelIdView id) noexcept;
        [[nodiscard]] const lux::ui::Panel* find(PanelIdView id) const noexcept;
        [[nodiscard]] std::vector<EditorPanelSnapshot> snapshot() const;
        [[nodiscard]] std::size_t close() noexcept;

    private:
        friend class EditorPanelInstallContext;
        struct Request;
        [[nodiscard]] lux::cxx::expected<void, EEditorPanelInstallError>
        addBatch(std::vector<Request> requests);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// One direct extension-entrypoint invocation. It is only a temporary
    /// argument list: no published descriptor catalog or lifecycle graph.
    class LUX_EDITOR_PUBLIC EditorPanelInstallContext final
    {
    public:
        explicit EditorPanelInstallContext(
            lux::extensions::ModuleLease module) noexcept;
        ~EditorPanelInstallContext();
        EditorPanelInstallContext(const EditorPanelInstallContext&) = delete;
        EditorPanelInstallContext& operator=(
            const EditorPanelInstallContext&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EEditorPanelInstallError> add(
            EditorPanelSpec spec,
            std::unique_ptr<lux::ui::Panel> panel);
        [[nodiscard]] lux::cxx::expected<void, EEditorPanelInstallError>
        commit(EditorPanels& panels) &&;

    private:
        lux::extensions::ModuleLease module_;
        std::vector<EditorPanels::Request> requests_;
    };
} // namespace lux::editor
