#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/error/detail/DefinitionTable.hpp>
#include <bit>
#include <charconv>
#include <mutex>
#include <shared_mutex>

namespace lux::error
{
    namespace
    {
        constexpr ErrorDescriptor RegistrationFailure{
            "lux.error.registration", "Error descriptor {0} rejected: registration code {1}", ERecovery::BUG,
            {EArgument::HEX, EArgument::UNSIGNED}
        };

        bool valid(const ErrorDescriptor& descriptor) noexcept
        {
            if (descriptor.name.empty() || errorId(descriptor.name) == 0)
                return false;
            for (const auto c : descriptor.name)
            {
                const bool is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                const bool is_digit = c >= '0' && c <= '9';
                const bool is_separator = c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
                if (!(is_letter || is_digit || is_separator))
                    return false;
            }
            if (descriptor.recovery > ERecovery::BUG)
                return false;
            unsigned declared{}, used{};
            for (std::size_t i{}; i < descriptor.arguments.size(); ++i)
            {
                if (descriptor.arguments[i] > EArgument::HEX)
                    return false;
                if (descriptor.arguments[i] != EArgument::NONE)
                    declared |= 1U << i;
            }
            const auto text = descriptor.message;
            for (std::size_t i{}; i < text.size(); ++i)
            {
                const char c = text[i];
                if (c != '{' && c != '}')
                    continue;
                if (i + 1 < text.size() && text[i + 1] == c)
                {
                    ++i;
                    continue;
                }
                const bool has_placeholder = c == '{' && i + 2 < text.size() && text[i + 2] == '}';
                if (!has_placeholder)
                    return false;
                const char index = text[i + 1];
                if (index < '0' || index > '2')
                    return false;
                used |= 1U << (index - '0');
                i += 2;
            }
            return used == declared;
        }

        void appendNumber(std::string& output, std::uint64_t value, EArgument kind) noexcept
        {
            char buffer[32];
            if (kind == EArgument::HEX)
                output += "0x";
            const auto converted = kind == EArgument::SIGNED ?
                std::to_chars(buffer, buffer + sizeof(buffer), std::bit_cast<std::int64_t>(value)) :
                std::to_chars(buffer, buffer + sizeof(buffer), value, kind == EArgument::HEX ? 16 : 10);
            output.append(buffer, converted.ptr);
        }
    }

    struct ErrorRegistry::Impl final
    {
        mutable std::shared_mutex mutex;
        detail::DefinitionTable table;
    };

    ErrorRegistry::ErrorRegistry() noexcept : impl_(std::make_unique<Impl>())
    {
        static_cast<void>(impl_->table.insert(errorId(RegistrationFailure.name), RegistrationFailure));
    }
    ErrorRegistry::~ErrorRegistry() = default;
    ErrorRegistry& ErrorRegistry::instance() noexcept
    {
        static ErrorRegistry registry;
        return registry;
    }
    cxx::expected<ErrorId, ERegistrationError> ErrorRegistry::registerType(const ErrorDescriptor& descriptor) noexcept
    {
        if (!valid(descriptor))
            return cxx::unexpected(ERegistrationError::INVALID_DESCRIPTOR);
        const auto id = errorId(descriptor.name);
        std::unique_lock lock(impl_->mutex);
        return impl_->table.insert(id, descriptor);
    }
    const ErrorDefinition* ErrorRegistry::find(ErrorId id) const noexcept
    {
        std::shared_lock lock(impl_->mutex);
        return impl_->table.find(id);
    }
    Error makeError(const ErrorDescriptor& descriptor, std::array<std::uint64_t, 3> args) noexcept
    {
        const auto registered = ErrorRegistry::instance().registerType(descriptor);
        if (!registered)
            return {errorId(RegistrationFailure.name), {errorId(descriptor.name), static_cast<std::uint64_t>(registered.error())}};
        return {*registered, args};
    }
    std::string format(Error error) noexcept
    {
        if (!error.type)
            return "No error";
        const auto* definition = ErrorRegistry::instance().find(error.type);
        if (!definition)
        {
            std::string result{"Unknown error "};
            appendNumber(result, error.type, EArgument::HEX);
            for (auto value : error.args)
            {
                result += " ";
                appendNumber(result, value, EArgument::HEX);
            }
            return result;
        }
        const auto text = std::string_view(definition->message);
        std::string result;
        result.reserve(text.size() + 96);
        for (std::size_t i{}; i < text.size(); ++i)
        {
            if (text[i] != '{' && text[i] != '}')
            {
                result += text[i];
                continue;
            }
            if (text[i + 1] == text[i])
            {
                result += text[i++];
                continue;
            }
            const auto argument = static_cast<std::size_t>(text[i + 1] - '0');
            appendNumber(result, error.args[argument], definition->arguments[argument]);
            i += 2;
        }
        return result;
    }
}
