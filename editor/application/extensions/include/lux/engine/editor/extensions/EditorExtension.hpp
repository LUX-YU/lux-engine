#pragma once
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/extensions/EditorExtensionAbi.hpp>
#include <lux/engine/editor/extensions/ExtensionCapabilities.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <thread>

namespace lux::editor::extensions
{
    inline constexpr std::uint32_t kEditorExtensionVersion = 8;
    inline constexpr const char* kEditorExtensionSymbol = "lux_editor_exports_v8";
    struct ContributionCounts final
    {
        std::uint32_t commands{}, sessions{}, views{}, configurations{}, components{}, reflection{};
        friend bool operator==(ContributionCounts, ContributionCounts) = default;
    };
    struct EditorExtensionExports final
    {
        std::uint32_t structure_size{sizeof(EditorExtensionExports)};
        std::uint32_t interface_version{kEditorExtensionVersion};
        const char* editor_sdk_abi{kEditorExtensionAbi};
        ContributionCounts counts;
        // Caller owns the draft; no catalog is modified by this entry. The supplied lease must wrap
        // every dynamic entry/payload/destructor, including later worker and owner-stage preparations.
        ContributionResult<void> (*contribute)(ContributionDraft&, contracts::CodeLease){};
        // Optional application activation, separate from cold metadata registration. No live Pane or
        // asynchronous work is published here. Captured activation state belongs to the returned
        // contribution closures and is destroyed before their external code lease.
        ContributionCounts activation_counts;
        ExtensionRequirements requires_capabilities;
        ContributionResult<void> (*activate)(ContributionDraft&, contracts::CodeLease, const ExtensionCapabilities&){};
    };
    using GetEditorExtension = const EditorExtensionExports*() noexcept;
    class EditorExtension final
    {
    public:
        [[nodiscard]] static lux::project::PluginResult<EditorExtension> load(
            const lux::project::PluginDescription&,
            const lux::project::PluginLibrary& runtime,
            std::span<const EditorExtension> dependencies = {}
        );
        [[nodiscard]] ContributionResult<ContributionDraft> contributions() const;
        [[nodiscard]] ContributionResult<ContributionDraft> activate(const ExtensionCapabilities&) const;
        [[nodiscard]] const lux::project::MetadataIdentity& identity() const noexcept;
        [[nodiscard]] const std::shared_ptr<const void>& code() const noexcept;

    private:
        lux::project::MetadataIdentity identity_;
        std::shared_ptr<const void> code_;
        const EditorExtensionExports* exports_{};
        std::thread::id owner_{std::this_thread::get_id()};
    };
}
