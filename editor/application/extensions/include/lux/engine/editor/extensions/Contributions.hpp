#pragma once
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/scene/InspectorComponent.hpp>
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <lux/engine/object/LuxObject.hpp>

namespace lux::editor::extensions
{
    enum class EContributionError : std::uint8_t
    {
        INVALID_ARGUMENT,
        DUPLICATE,
        CAPACITY,
        BUSY,
        WRONG_THREAD,
        CALLBACK,
        INCOMPATIBLE_ABI,
        IO,
        UNAVAILABLE
    };
    struct ContributionFailure final
    {
        EContributionError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using ContributionResult = cxx::expected<T, ContributionFailure>;
    struct ReflectionContribution final
    {
        lux::object::CodeLease code;
        meta::ReflectionRegistrationDraft::RegisterFn register_types{};
    };
    // Mutable preparation only. Providers write their own lower-layer entries, never borrow old Context.
    struct ContributionDraft final
    {
        ContributionDraft() = default;
        ContributionDraft(ContributionDraft&&) noexcept = default;
        ContributionDraft& operator=(ContributionDraft&&) = delete;
        ContributionDraft(const ContributionDraft&) = delete;
        ContributionDraft& operator=(const ContributionDraft&) = delete;
        // External draft-level pin also covers rejected entries before catalog normalization.
        std::vector<lux::object::CodeLease> code;
        std::vector<ReflectionContribution> reflection;
        std::vector<std::shared_ptr<const services::ServiceEntry>> services;
        std::vector<std::shared_ptr<const desktop::UiEntry>> ui;
        std::vector<std::shared_ptr<commands::CommandEntry>> commands;
        std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> sessions;
        std::vector<std::shared_ptr<views::ViewFactoryEntry>> views;
        std::vector<scene::ConfigurationEditor> configurations;
        std::vector<scene::InspectorComponent> components;
        std::vector<settings::SettingsPage> settings;
    };
    class ContributionSnapshot final
    {
    public:
        [[nodiscard]] static ContributionResult<ContributionSnapshot> prepare(
            ContributionDraft,
            std::size_t capacity = 256
        );
        [[nodiscard]] const commands::CommandRegistrySnapshot& commands() const noexcept;
        [[nodiscard]] std::span<const std::shared_ptr<const services::ServiceEntry>> services() const noexcept;
        [[nodiscard]] const desktop::UiCatalog& ui() const noexcept;
        [[nodiscard]] const sessions::SessionFactorySnapshot& sessions() const noexcept;
        [[nodiscard]] const views::ViewFactorySnapshot& views() const noexcept;
        [[nodiscard]] std::span<const scene::ConfigurationEditor> configurations() const noexcept;
        [[nodiscard]] std::span<const scene::InspectorComponent> components() const noexcept;
        [[nodiscard]] std::span<const settings::SettingsPage> settings() const noexcept;
        // Cold external-name boundary. Drafts and pages retain the resulting entry, not its name.
        [[nodiscard]] const settings::SettingsPage* findSetting(settings::SettingsIdView) const noexcept;
        [[nodiscard]] bool valid() const noexcept;

    private:
        friend class ContributionRegistry;
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    // Application composition only. Queue updates during callbacks; publish one complete candidate at
    // the outer safe point. No producer is informed until every catalog has switched and old owners retire.
    class ContributionRegistry final : public object::LuxObject
    {
    public:
        object::TSignal<std::uint64_t> changed{*this};
        ContributionRegistry(object::ObjectDispatcherRef, desktop::EditorContext&, std::size_t pending_capacity = 8);
        ~ContributionRegistry() override;
        ContributionRegistry(const ContributionRegistry&) = delete;
        ContributionRegistry& operator=(const ContributionRegistry&) = delete;
        ContributionRegistry(ContributionRegistry&&) = delete;
        ContributionRegistry& operator=(ContributionRegistry&&) = delete;
        [[nodiscard]] ContributionResult<void> enqueue(ContributionSnapshot&);
        [[nodiscard]] ContributionResult<object::SignalDelivery> applyPending();
        // One fixed snapshot for the entire factory/install batch. Callbacks may enqueue the next batch,
        // but recursive adoption/publication is rejected; accepted task completions remain in their owners.
        [[nodiscard]] ContributionResult<
            void> withSnapshot(cxx::function_ref<ContributionResult<void>(const ContributionSnapshot&)>);
        [[nodiscard]] ContributionSnapshot snapshot() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::extensions
