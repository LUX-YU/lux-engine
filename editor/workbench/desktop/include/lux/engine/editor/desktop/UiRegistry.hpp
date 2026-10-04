#pragma once

#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <optional>

namespace lux::editor::desktop
{
    enum class EUiError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        INVALID_CONFIGURATION,
        INVALID_OUTPUT,
        CAPACITY,
        DUPLICATE,
        HASH_COLLISION,
        NOT_FOUND,
        STALE_REGISTRATION,
        STALE_ROOT,
        ATTACHMENT,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        DEPENDENCY,
        FACTORY_FAILURE
    };
    struct UiFailure final
    {
        EUiError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using UiResult = cxx::expected<T, UiFailure>;
    // Existing workspace state schema and explicit content association, never an asset from layout opaque.
    struct UiCreateInfo final
    {
        object::ObjectDispatcherRef dispatcher;
        lux::ui::PaneId instance;
        views::ViewContent content;
        workspace::VersionedViewState configuration;
    };
    struct UiDescriptor final
    {
        views::ViewTypeIdView type;
        std::string_view label;
        std::span<const services::ServiceDependency> dependencies;
        std::uint32_t schema{1};
        UiResult<void> (*validate)(std::span<const std::byte>) noexcept {};
        UiResult<std::unique_ptr<lux::ui::Pane>> (*create)(services::ServiceResolver&, const UiCreateInfo&){};
    };
    class UiEntry final
    {
    public:
        template <auto& Descriptor>
            requires std::same_as<std::remove_cvref_t<decltype(Descriptor)>, UiDescriptor> &&
                     std::is_const_v<std::remove_reference_t<decltype(Descriptor)>>
        [[nodiscard]] static std::shared_ptr<const UiEntry> bind(object::CodeLease code)
        {
            auto entry = std::shared_ptr<const UiEntry>(new UiEntry(code, Descriptor));
            return object::pinCodeOwner(std::move(code), std::move(entry));
        }
        [[nodiscard]] static std::shared_ptr<const UiEntry> create(object::CodeLease, const UiDescriptor&);
        ~UiEntry();
        UiEntry(const UiEntry&) = delete;
        UiEntry& operator=(const UiEntry&) = delete;
        UiEntry(UiEntry&&) = delete;
        UiEntry& operator=(UiEntry&&) = delete;
        [[nodiscard]] const UiDescriptor& descriptor() const noexcept
        {
            return *descriptor_;
        }
        [[nodiscard]] const object::CodeLease& code() const noexcept
        {
            return code_;
        }

    private:
        UiEntry(object::CodeLease, const UiDescriptor&);
        struct Storage;
        object::CodeLease code_;
        std::unique_ptr<const Storage> storage_;
        const UiDescriptor* descriptor_;
    };
    class UiHandle final
    {
    public:
        [[nodiscard]] bool valid() const noexcept
        {
            return bool(entry_);
        }
        [[nodiscard]] const UiDescriptor& descriptor() const noexcept;

    private:
        friend class UiCatalog;
        friend class UiRegistry;
        std::shared_ptr<const UiEntry> entry_;
    };
    class UiCatalog final
    {
    public:
        [[nodiscard]] static UiResult<UiCatalog> prepare(
            std::vector<std::shared_ptr<const UiEntry>>,
            std::size_t capacity = 256
        );
        [[nodiscard]] UiResult<UiHandle> find(views::ViewTypeIdView) const noexcept;
        [[nodiscard]] UiResult<UiHandle> at(std::size_t) const noexcept;
        [[nodiscard]] std::span<const std::shared_ptr<const UiEntry>> entries() const noexcept;

    private:
        friend class UiRegistry;
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    // A resolved factory and an owned capture of its original configuration/content association.
    // The source workspace/recovery document is not consumed or rewritten by a failed mount.
    struct UiMountRequest final
    {
        UiHandle factory;
        UiCreateInfo input;
        bool visible{true};
    };
    // Owns immutable factory metadata only. A successful creation is a standard unique owner;
    // Root's Object ownership relation takes over when the caller mounts the complete candidate.
    class UiRegistry final
    {
    public:
        class ReadScope final
        {
        public:
            ~ReadScope();
            ReadScope(ReadScope&&) noexcept;
            ReadScope(const ReadScope&) = delete;
            ReadScope& operator=(const ReadScope&) = delete;
            ReadScope& operator=(ReadScope&&) = delete;

        private:
            friend class UiRegistry;
            explicit ReadScope(UiRegistry&) noexcept;
            UiRegistry* owner_;
        };
        [[nodiscard]] UiResult<ReadScope> readScope() noexcept;
        class Publication final
        {
        public:
            ~Publication();
            Publication(Publication&&) noexcept;
            Publication(const Publication&) = delete;
            Publication& operator=(const Publication&) = delete;
            Publication& operator=(Publication&&) = delete;
            void commit() noexcept;
            // Clean abandoned/retired code before any participant releases its publication guard.
            // An uncommitted permission is consumed; protection continues until destruction.
            void clearRetained() noexcept;

        private:
            friend class UiRegistry;
            struct State;
            explicit Publication(std::unique_ptr<State>) noexcept;
            std::unique_ptr<State> state_;
        };
        UiRegistry(object::ObjectDispatcherRef, services::ServiceRegistry&);
        ~UiRegistry();
        UiRegistry(const UiRegistry&) = delete;
        UiRegistry& operator=(const UiRegistry&) = delete;
        UiRegistry(UiRegistry&&) = delete;
        UiRegistry& operator=(UiRegistry&&) = delete;
        [[nodiscard]] UiResult<Publication> preparePublication(UiCatalog) noexcept;
        [[nodiscard]] UiResult<void> publish(UiCatalog) noexcept;
        [[nodiscard]] UiCatalog snapshot() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>>
        create(const UiHandle&, services::ServiceScope&, const UiCreateInfo&) noexcept;
        // One synchronous construction/commit boundary shared by configuration, menu and recovery.
        // No retained window table: successful owners transfer directly to Root's Object relation.
        [[nodiscard]] UiResult<lux::ui::AttachmentCommit> mount(
            lux::ui::Root&,
            services::ServiceScope&,
            std::vector<UiMountRequest>,
            std::optional<lux::ui::DockTree> = {}
        ) noexcept;

    private:
        [[nodiscard]] UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>>
        createImpl(const UiHandle&, services::ServiceScope&, const UiCreateInfo&) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
