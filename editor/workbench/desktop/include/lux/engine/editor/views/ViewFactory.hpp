#pragma once
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <span>

namespace lux::editor::views
{
    enum class EViewFactoryError : std::uint8_t
    {
        INVALID_ARGUMENT,
        NOT_FOUND,
        CAPACITY,
        CONSTRUCT,
        CALLBACK
    };
    struct ViewFactoryFailure final
    {
        EViewFactoryError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using ViewFactoryResult = cxx::expected<T, ViewFactoryFailure>;
    struct ViewFactoryDescriptor final
    {
        ViewTypeId type;
        std::string label;
        cxx::TypeToken binding_type;
        std::uint32_t input_version{1};
    };
    // Immutable typed binding. The external code pin survives payload destruction and replacement.
    // Contents are identities and explicit borrowed services; never a mutable author source.
    class ViewFactoryInput final
    {
    public:
        ViewFactoryInput(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            contracts::CodeLease,
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
        ViewFactoryEntry(contracts::CodeLease, ViewFactoryDescriptor, Create);
        ~ViewFactoryEntry();
        ViewFactoryEntry(const ViewFactoryEntry&) = delete;
        ViewFactoryEntry& operator=(const ViewFactoryEntry&) = delete;
        ViewFactoryEntry(ViewFactoryEntry&&) = delete;
        ViewFactoryEntry& operator=(ViewFactoryEntry&&) = delete;
        [[nodiscard]] const ViewFactoryDescriptor& descriptor() const noexcept;
        [[nodiscard]] bool usesCode(const contracts::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }

    private:
        friend class ViewFactorySnapshot;
        contracts::CodeLease code_;
        ViewFactoryDescriptor descriptor_;
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
        [[nodiscard]] std::span<const std::shared_ptr<ViewFactoryEntry>> entries() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
}
