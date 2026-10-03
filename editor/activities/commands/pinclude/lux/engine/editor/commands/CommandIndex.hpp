#pragma once
#include <lux/engine/editor/commands/Command.hpp>
#include <algorithm>
#include <span>
#include <vector>

namespace lux::editor::commands::detail
{
    struct CommandIndex final
    {
        std::uint64_t hash;
        std::size_t entry;
    };
    // Both production and collision fixtures use this same candidate-index algorithm.
    // Hash is a projection of the already verified identity; it never dispatches a handler.
    template <class Entries, class Hash>
    CommandResult<std::vector<CommandIndex>> commandIndex(const Entries& entries, Hash hash)
    {
        std::vector<CommandIndex> result;
        result.reserve(entries.size());
        for (std::size_t i{}; i < entries.size(); ++i)
            result.push_back({hash(entries[i]->descriptor().id), i});
        std::ranges::sort(result, {}, &CommandIndex::hash);
        for (std::size_t i = 1; i < result.size(); ++i)
        {
            if (result[i - 1].hash != result[i].hash)
                continue;
            const auto first = entries[result[i - 1].entry]->descriptor().id.name();
            const auto second = entries[result[i].entry]->descriptor().id.name();
            const auto code = first == second ? ECommandError::DUPLICATE : ECommandError::HASH_COLLISION;
            return cxx::unexpected(CommandFailure{
                code, "command.identity", 0, std::string{first} + " / " + std::string{second}
            });
        }
        return result;
    }
}
