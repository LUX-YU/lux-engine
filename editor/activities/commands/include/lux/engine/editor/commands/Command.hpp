#pragma once

#include <lux/engine/editor/contracts/CodeLease.hpp>
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <optional>
#include <variant>

namespace lux::ui
{
    struct CommandIdTag;
}

namespace lux::editor::commands
{
    // The same semantic identity as UI commands, without a dependency on Menu or the UI library.
    using CommandId = cxx::StableNameId<lux::ui::CommandIdTag>;
    using CommandIdView = cxx::StableNameIdView<lux::ui::CommandIdTag>;

    enum class ECommandError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        BUSY,
        CAPACITY,
        NOT_FOUND,
        STALE_TARGET,
        STALE_CONTENT,
        INCOMPATIBLE_REGISTRATION,
        DISABLED,
        CLOSED,
        DOMAIN_FAILURE,
        DUPLICATE,
        HASH_COLLISION,
        SHORTCUT_CONFLICT
    };
    struct CommandFailure final
    {
        ECommandError code;
        std::string domain;
        std::uint64_t domain_code{};
        std::string detail;
    };
    template <class T> using CommandResult = cxx::expected<T, CommandFailure>;

    enum class ECommandScope : std::uint8_t
    {
        APPLICATION,
        SESSION,
        VIEW
    };
    enum class ERegistryBinding : std::uint8_t
    {
        PINNED,
        CURRENT_REGISTRATION
    };
    struct CommandDescriptor final
    {
        CommandIdView id;
        std::string_view label;
        std::string_view group;
        // Canonical default binding; parsed once by CommandEntry, never by event dispatch.
        std::string_view shortcut;
        ECommandScope scope{ECommandScope::APPLICATION};
        std::uint32_t input_version{1};
        cxx::TypeToken argument_type;
    };
    struct ShortcutOverride final
    {
        std::string command;
        std::string binding;
        ECommandScope scope{ECommandScope::APPLICATION};
        std::uint32_t input_version{1};
    };
    struct SessionTarget final
    {
        sessions::SessionId id;
        // Present only for operations whose meaning requires this exact author version.
        std::optional<sessions::ContentStamp> based_on;
        friend bool operator==(const SessionTarget&, const SessionTarget&) = default;
    };
    using VCommandTarget = std::variant<std::monostate, SessionTarget, views::ViewId>;

    // One owning erased value. Code outlives the payload, including replacement and rejected admission.
    class CommandArguments final
    {
    public:
        CommandArguments() noexcept = default;
        CommandArguments(contracts::CodeLease, cxx::TypeToken, std::shared_ptr<const void>);
        ~CommandArguments();
        CommandArguments(const CommandArguments&) noexcept = default;
        CommandArguments(CommandArguments&&) noexcept = default;
        CommandArguments& operator=(CommandArguments) noexcept;
        [[nodiscard]] cxx::TypeToken type() const noexcept;
        [[nodiscard]] const void* data() const noexcept;
        [[nodiscard]] bool valid() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    struct CommandQuery final
    {
        const VCommandTarget& target;
        const CommandArguments& arguments;
    };
    struct CommandState final
    {
        bool enabled{};
        bool checked{};
        std::string reason;
    };
    class CommandInvocation final
    {
    public:
        explicit CommandInvocation(
            VCommandTarget target = {},
            CommandArguments arguments = {},
            ERegistryBinding registration = ERegistryBinding::PINNED
        ) noexcept;
        [[nodiscard]] const VCommandTarget& target() const noexcept;
        [[nodiscard]] const CommandArguments& arguments() const noexcept;
        [[nodiscard]] ERegistryBinding registration() const noexcept;
        [[nodiscard]] CommandQuery query() const noexcept;

    private:
        VCommandTarget target_;
        CommandArguments arguments_;
        ERegistryBinding registration_;
    };
    struct ImmediateCompletion final
    {};
    // An existing business identity for observation, never a second copy of its state or cancellation.
    struct OperationKindIdTag;
    using OperationKindId = cxx::StableNameId<OperationKindIdTag>;
    struct AcceptedOperation final
    {
        OperationKindId kind;
        std::uint64_t value{};
    };
    using DispatchReceipt = std::variant<ImmediateCompletion, AcceptedOperation>;
}
