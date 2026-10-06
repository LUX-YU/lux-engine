#pragma once

#include <lux/engine/error/ErrorRegistry.hpp>
#include <unordered_map>

namespace lux::error::detail
{
    // Locking and hashing belong to ErrorRegistry. Kept separate so collision handling can be
    // exercised with deterministic occupied buckets without changing the public hash contract.
    class DefinitionTable final
    {
    public:
        [[nodiscard]] cxx::expected<ErrorId, ERegistrationError>
        insert(ErrorId id, const ErrorDescriptor& descriptor) noexcept
        {
            auto existing = definitions_.find(id);
            if (existing != definitions_.end())
            {
                const auto& value = *existing->second;
                if (value.name != descriptor.name)
                    return cxx::unexpected(ERegistrationError::HASH_COLLISION);
                const bool is_same_definition = value.message == descriptor.message &&
                    value.recovery == descriptor.recovery && value.arguments == descriptor.arguments;
                if (!is_same_definition)
                    return cxx::unexpected(ERegistrationError::DEFINITION_MISMATCH);
                return id;
            }
            definitions_.emplace(id, std::make_unique<const ErrorDefinition>(ErrorDefinition{
                std::string(descriptor.name), std::string(descriptor.message), descriptor.recovery, descriptor.arguments
            }));
            return id;
        }

        [[nodiscard]] const ErrorDefinition* find(ErrorId id) const noexcept
        {
            const auto found = definitions_.find(id);
            return found == definitions_.end() ? nullptr : found->second.get();
        }

    private:
        std::unordered_map<ErrorId, std::unique_ptr<const ErrorDefinition>> definitions_;
    };
}
