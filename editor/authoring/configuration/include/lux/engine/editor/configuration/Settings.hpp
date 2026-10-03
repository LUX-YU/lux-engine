#pragma once
#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/editor/contracts/CodeLease.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/workspace/SettingsDocument.hpp>
#include <optional>

namespace lux::editor::settings
{
    struct SettingsIdTag;
    using SettingsIdView = cxx::StableNameIdView<SettingsIdTag>;

    enum class ESettingsApply : std::uint8_t { IMMEDIATE, SAFE_POINT, RESTART };

    // The original codec owns default construction/encoding. A null merge means whole-value replacement.
    // Optional migrations are explicit: a schema mismatch never silently decodes as the current schema.
    struct SettingsDescriptor final
    {
        SettingsIdView id;
        std::string_view label;
        const ConfigurationDescriptor* configuration{};
        std::uint32_t scopes{kPersonalScopes};
        ESettingsApply apply{ESettingsApply::RESTART};
        SettingsResult<void> (*validate)(const ConfigurationValue&) noexcept {};
        SettingsResult<void> (*merge)(ConfigurationValue&, const ConfigurationValue&) noexcept {};
        SettingsResult<std::vector<std::byte>> (*migrate)(std::uint32_t, std::span<const std::byte>) noexcept {};
    };

    class LUX_EDITOR_CONFIGURATION_PUBLIC SettingsEntry final
    {
    public:
        using Apply = cxx::move_only_function<SettingsResult<void>(const ConfigurationValue&)>;
        template <const SettingsDescriptor& Descriptor>
        [[nodiscard]] static std::shared_ptr<SettingsEntry> bind(contracts::CodeLease code, Apply apply = {})
        {
            static_assert(Descriptor.id.isValid() && !Descriptor.label.empty() && Descriptor.configuration);
            return std::shared_ptr<SettingsEntry>(new SettingsEntry(std::move(code), Descriptor, std::move(apply)));
        }
        // One immutable backing owns all dynamic names and the original configuration descriptor.
        [[nodiscard]] static std::shared_ptr<SettingsEntry> create(contracts::CodeLease, const SettingsDescriptor&, Apply = {});
        ~SettingsEntry();
        SettingsEntry(const SettingsEntry&) = delete;
        SettingsEntry& operator=(const SettingsEntry&) = delete;
        SettingsEntry(SettingsEntry&&) = delete;
        SettingsEntry& operator=(SettingsEntry&&) = delete;
        [[nodiscard]] const SettingsDescriptor& descriptor() const noexcept;
        [[nodiscard]] const contracts::CodeLease& code() const noexcept { return code_; }
        [[nodiscard]] bool usesCode(const contracts::CodeLease&) const noexcept;
        [[nodiscard]] SettingsResult<void> validateDescriptor() const noexcept;
        [[nodiscard]] SettingsResult<ConfigurationValue> defaults() const noexcept;
        [[nodiscard]] SettingsResult<void> validateDefault(meta::ReflectionRegistry&) const noexcept;
        [[nodiscard]] SettingsResult<ConfigurationValue> decode(std::uint32_t, std::span<const std::byte>) const noexcept;
        [[nodiscard]] SettingsResult<void> validate(const ConfigurationValue&) const noexcept;
        // Called by the exact owner at its documented safe point/startup, never implicitly by parsing.
        [[nodiscard]] SettingsResult<void> apply(const ConfigurationValue&);
        [[nodiscard]] bool hasApply() const noexcept { return bool(apply_); }

    private:
        SettingsEntry(contracts::CodeLease, const SettingsDescriptor&, Apply);
        [[nodiscard]] SettingsResult<ConfigurationValue> defaults(meta::ReflectionRegistry&) const noexcept;
        struct Storage;
        contracts::CodeLease code_;
        std::unique_ptr<const Storage> storage_;
        const SettingsDescriptor* descriptor_;
        Apply apply_;
    };
    // Pure cold validation; ContributionSnapshot is the sole publication owner.
    [[nodiscard]] LUX_EDITOR_CONFIGURATION_PUBLIC SettingsResult<void>
    validateSettingsEntries(std::span<const std::shared_ptr<SettingsEntry>>, std::size_t capacity = 256);

    struct SettingsResolution final
    {
        std::shared_ptr<const SettingsEntry> entry;
        ConfigurationValue desired;
        std::optional<ESettingsScope> source;
    };
    // Sources may be supplied in any order, but each scope occurs at most once.
    // Default < installation < project < user < user-project < launch, constrained per descriptor.
    [[nodiscard]] LUX_EDITOR_CONFIGURATION_PUBLIC SettingsResult<SettingsResolution>
    resolveSettings(std::shared_ptr<const SettingsEntry>, std::span<const SettingsDocument>);

    struct SettingsDraft final
    {
        std::shared_ptr<const SettingsEntry> entry;
        ESettingsScope scope;
        std::string based_on;
        ConfigurationValue desired;
        // These are owner-confirmed facts, not a combined dirty flag. Preparing a file never sets them.
        std::optional<std::vector<std::byte>> applied;
        std::optional<std::vector<std::byte>> persisted;
    };
    [[nodiscard]] LUX_EDITOR_CONFIGURATION_PUBLIC SettingsResult<SettingsDraft>
    makeSettingsDraft(const SettingsResolution&, const SettingsDocument&);
    [[nodiscard]] LUX_EDITOR_CONFIGURATION_PUBLIC SettingsResult<SettingsDocument>
    prepareSettings(const SettingsDraft&, const SettingsDocument&, const SettingsEntry& current);
}
