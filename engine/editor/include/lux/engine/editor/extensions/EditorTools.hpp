#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/ecs/TypeToken.hpp>
#include <lux/engine/editor/visibility.h>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/OperationTicket.hpp>
#include <lux/engine/core/extension_abi/StableId.hpp>
#include <lux/engine/ui/UISystem.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::events
{
    class DomainEvents;
}

namespace lux::editor
{
    enum class EEditorServiceRegistrationError : std::uint8_t
    {
        INVALID_SERVICE,
        DUPLICATE_TYPE,
        TYPE_HASH_COLLISION
    };

    class EditorPanelCreateContext final
    {
    public:
        template <class T>
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorServiceRegistrationError>
        add(T& service)
        {
            return addErased(Entry{
                lux::ecs::typeToken<T>(),
                std::addressof(service),
                {}});
        }

        template <class T>
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorServiceRegistrationError>
        addShared(std::shared_ptr<T> service)
        {
            if (!service)
            {
                return lux::cxx::unexpected(
                    EEditorServiceRegistrationError::INVALID_SERVICE);
            }
            auto* value = service.get();
            return addErased(Entry{
                lux::ecs::typeToken<T>(),
                value,
                std::move(service)});
        }

        template <class T>
        [[nodiscard]] T* find() const noexcept
        {
            constexpr auto type = lux::ecs::typeToken<T>();
            for (const auto& entry : entries_)
                if (lux::ecs::sameTypeToken(entry.type, type))
                    return static_cast<T*>(entry.value);
            return nullptr;
        }

        template <class T>
        [[nodiscard]] std::shared_ptr<T> findShared() const noexcept
        {
            constexpr auto type = lux::ecs::typeToken<T>();
            for (const auto& entry : entries_)
            {
                if (lux::ecs::sameTypeToken(entry.type, type) && entry.owner)
                    return std::shared_ptr<T>{
                        entry.owner,
                        static_cast<T*>(entry.value)};
            }
            return {};
        }

        [[nodiscard]] bool contains(lux::ecs::TypeToken type) const noexcept;

    private:
        friend class EditorToolHost;
        struct Entry final
        {
            lux::ecs::TypeToken type{};
            void* value{nullptr};
            std::shared_ptr<void> owner;
        };
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorServiceRegistrationError>
        addErased(Entry entry);
        std::vector<Entry> entries_;
    };

    enum class EEditorPanelCreateError : std::uint8_t
    {
        REQUIRED_SERVICE_MISSING,
        CREATE_FAILED
    };

    struct EditorPanelContributionDescriptor final
    {
        EditorPanelContributionDescriptor() = default;
        EditorPanelContributionDescriptor(
            const EditorPanelContributionDescriptor&) = delete;
        EditorPanelContributionDescriptor& operator=(
            const EditorPanelContributionDescriptor&) = delete;
        EditorPanelContributionDescriptor(
            EditorPanelContributionDescriptor&&) noexcept = default;
        EditorPanelContributionDescriptor& operator=(
            EditorPanelContributionDescriptor&&) noexcept = default;

        lux::extensions::ContributionId id;
        std::string display_name;
        bool default_visible{true};
        /// Some process-composition panels are borrowed by long-lived
        /// controllers. They may be hidden but cannot be destroyed before the
        /// editor composition closes those borrowers.
        bool supports_deactivation{true};
        std::vector<lux::ecs::TypeToken> required_editor_services;
        lux::cxx::move_only_function<
            lux::cxx::expected<
                std::unique_ptr<lux::ui::Panel>,
                EEditorPanelCreateError>(const EditorPanelCreateContext&)>
            create;
        lux::extensions::ExtensionId provider;
        lux::extensions::ModuleLease module;
    };

    enum class EEditorPanelCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_CONTRIBUTION,
        ID_COLLISION,
        MISSING_CREATE_CALLBACK
    };

    class LUX_EDITOR_PUBLIC EditorPanelCatalog final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, EEditorPanelCatalogError> add(
            EditorPanelContributionDescriptor descriptor);
        [[nodiscard]] lux::cxx::expected<void, EEditorPanelCatalogError>
        validateBatch(
            std::span<const EditorPanelContributionDescriptor> descriptors)
            const noexcept;
        [[nodiscard]] lux::cxx::expected<void, EEditorPanelCatalogError>
        addBatch(std::vector<EditorPanelContributionDescriptor> descriptors);
        [[nodiscard]] EditorPanelContributionDescriptor* find(
            lux::extensions::ContributionIdView id) noexcept;
        [[nodiscard]] const EditorPanelContributionDescriptor* find(
            lux::extensions::ContributionIdView id) const noexcept;
        [[nodiscard]] std::span<const EditorPanelContributionDescriptor> all()
            const noexcept;

    private:
        std::vector<EditorPanelContributionDescriptor> descriptors_;
    };

    enum class EEditorToolPhase : std::uint8_t
    {
        QUEUED,
        CREATING,
        REGISTERING_UI,
        ACTIVE,
        UPDATING_VISIBILITY,
        DEACTIVATING,
        INACTIVE
    };

    enum class EEditorToolError : std::uint8_t
    {
        NONE,
        QUEUE_FULL,
        STOPPING,
        UNKNOWN_CONTRIBUTION,
        REQUIRED_SERVICE_MISSING,
        CREATE_FAILED,
        UI_REGISTRATION_FAILED,
        DEACTIVATION_NOT_SUPPORTED,
        NOT_ACTIVE
    };

    struct EditorToolResult final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
        bool visible{false};
    };

    using EditorToolTicket = lux::extensions::OperationTicket<
        EEditorToolPhase,
        EEditorToolError,
        EditorToolResult>;

    struct EditorToolStateChanged final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
        bool visible{false};
    };

    struct EditorToolSnapshot final
    {
        lux::extensions::ContributionId contribution;
        std::string display_name;
        bool default_visible{true};
        bool active{false};
        bool visible{false};
    };

    namespace detail
    {
        struct EditorToolEndpoint;
    }

    class LUX_EDITOR_PUBLIC EditorTools final
    {
    public:
        EditorTools() noexcept = default;

        [[nodiscard]] EditorToolTicket requestOpen(
            lux::extensions::ContributionIdView id) const;
        [[nodiscard]] EditorToolTicket requestVisible(
            lux::extensions::ContributionIdView id,
            bool visible) const;
        [[nodiscard]] EditorToolTicket requestDeactivate(
            lux::extensions::ContributionIdView id) const;
        [[nodiscard]] std::vector<EditorToolSnapshot> snapshot() const;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class EditorToolHost;
        explicit EditorTools(
            std::shared_ptr<detail::EditorToolEndpoint> endpoint) noexcept;

        std::shared_ptr<detail::EditorToolEndpoint> endpoint_;
    };

    class LUX_EDITOR_PUBLIC EditorToolHost final
    {
    public:
        EditorToolHost(
            lux::ui::UISystem& ui,
            EditorPanelCatalog& catalog,
            EditorPanelCreateContext context,
            lux::events::DomainEvents* events = nullptr,
            std::size_t queue_capacity = 64u);
        ~EditorToolHost() noexcept;
        EditorToolHost(const EditorToolHost&) = delete;
        EditorToolHost& operator=(const EditorToolHost&) = delete;

        [[nodiscard]] EditorTools facade() const noexcept;
        template <class T>
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorServiceRegistrationError>
        addService(T& service)
        {
            return addServiceErased(
                lux::ecs::typeToken<T>(),
                std::addressof(service));
        }
        [[nodiscard]] std::size_t processSafePoint(
            std::size_t budget = 32u) noexcept;
        [[nodiscard]] std::vector<EditorToolSnapshot> snapshot() const;
        [[nodiscard]] lux::ui::Panel* activePanel(
            lux::extensions::ContributionIdView id) noexcept;
        [[nodiscard]] std::size_t close() noexcept;

    private:
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorServiceRegistrationError>
        addServiceErased(
            lux::ecs::TypeToken type,
            void* service);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
