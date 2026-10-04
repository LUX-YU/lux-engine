#pragma once
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/editor/sessions/SessionId.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <span>
#include <variant>

namespace lux::editor::views
{
    enum class EViewFactoryError : std::uint8_t
    {
        INVALID_ARGUMENT,
        NOT_FOUND,
        CAPACITY,
        CONSTRUCT,
        CALLBACK,
        AMBIGUOUS,
        BUSY,
        HASH_COLLISION
    };
    struct ViewFactoryFailure final
    {
        EViewFactoryError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using ViewFactoryResult = cxx::expected<T, ViewFactoryFailure>;
    struct ContentViewInput final
    {
        ViewContent content;
        std::string title;
    };
    // Standalone tool windows use typeToken<std::monostate>() and an owned monostate input.
    // The application can expose these in its Window menu without knowing plugin-specific services.
    struct ViewFactoryDescriptor final
    {
        ViewTypeIdView type;
        std::string_view label;
        cxx::TypeToken binding_type;
        std::uint32_t input_version{1};
        std::span<const sessions::SessionKindIdView> content_kinds;
        bool default_content_view{true};
    };
    // Immutable typed binding. The external code pin survives payload destruction and replacement.
    // Contents are identities and explicit borrowed services; never a mutable author source.
    class ViewFactoryInput final
    {
    public:
        ViewFactoryInput(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            lux::object::CodeLease,
            cxx::TypeToken,
            std::shared_ptr<const void> binding,
            std::uint32_t version = 1
        );
        ~ViewFactoryInput();
        ViewFactoryInput(const ViewFactoryInput&) noexcept = default;
        ViewFactoryInput(ViewFactoryInput&&) noexcept = default;
        ViewFactoryInput& operator=(ViewFactoryInput) noexcept;
        [[nodiscard]] object::ObjectDispatcherRef dispatcher() const noexcept;
        [[nodiscard]] const lux::ui::PaneId& paneId() const noexcept;
        [[nodiscard]] const void* binding() const noexcept;
        [[nodiscard]] cxx::TypeToken bindingType() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] bool valid() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    class ViewFactoryEntry final
    {
    public:
        using Create = cxx::move_only_function<ViewFactoryResult<DetachedView>(const ViewFactoryInput&)>;
        template <const ViewFactoryDescriptor& Descriptor>
        [[nodiscard]] static std::shared_ptr<ViewFactoryEntry> bind(lux::object::CodeLease code, Create create)
        {
            static_assert(
                Descriptor.type.isValid() && !Descriptor.label.empty() && Descriptor.binding_type.isValid() &&
                    Descriptor.input_version != 0,
                "Fixed view metadata must be a valid constant declaration."
            );
            return std::shared_ptr<ViewFactoryEntry>(
                new ViewFactoryEntry(std::move(code), Descriptor, std::move(create))
            );
        }
        // Freeze dynamic strings/arrays before publishing any borrowed descriptor.
        [[nodiscard]] static std::shared_ptr<ViewFactoryEntry> create(
            lux::object::CodeLease,
            const ViewFactoryDescriptor&,
            Create
        );
        ~ViewFactoryEntry();
        ViewFactoryEntry(const ViewFactoryEntry&) = delete;
        ViewFactoryEntry& operator=(const ViewFactoryEntry&) = delete;
        ViewFactoryEntry(ViewFactoryEntry&&) = delete;
        ViewFactoryEntry& operator=(ViewFactoryEntry&&) = delete;
        [[nodiscard]] const ViewFactoryDescriptor& descriptor() const noexcept;
        [[nodiscard]] bool usesCode(const lux::object::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }

    private:
        friend class ViewFactorySnapshot;
        struct DescriptorStorage;
        ViewFactoryEntry(lux::object::CodeLease, const ViewFactoryDescriptor&, Create);
        lux::object::CodeLease code_;
        std::unique_ptr<const DescriptorStorage> storage_;
        const ViewFactoryDescriptor* descriptor_;
        Create create_;
    };
    class ViewFactorySnapshot final
    {
    public:
        [[nodiscard]] static ViewFactoryResult<ViewFactorySnapshot> create(
            std::vector<std::shared_ptr<ViewFactoryEntry>>,
            std::size_t capacity = 256
        );
        [[nodiscard]] ViewFactoryResult<DetachedView> prepare(ViewTypeId, const ViewFactoryInput&) const;
        [[nodiscard]] ViewFactoryResult<ViewTypeId> selectContent(
            const sessions::SessionKindId&,
            std::optional<ViewTypeId> preferred = {}
        ) const;
        [[nodiscard]] std::span<const std::shared_ptr<ViewFactoryEntry>> entries() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
} // namespace lux::editor::views
