#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>

namespace lux::flowforge
{
    const ScriptAbilityNodeDescription* ScriptAbilityNodeCatalogView::find(
        lux::script::ScriptApiContractIdView contract,
        lux::script::ScriptApiMethodIdView method
    ) const noexcept
    {
        for (const auto& node : nodes_)
        {
            if (node.contract == contract && node.method == method)
                return &node;
        }
        return nullptr;
    }

    lux::cxx::expected<void, EScriptAbilityNodeCatalogError>
    ScriptAbilityNodeCatalog::add(ScriptAbilityCatalogContribution contribution) noexcept
    {
        for (std::size_t candidate_index{}; candidate_index < contribution.nodes.size(); ++candidate_index)
        {
            const auto& candidate = contribution.nodes[candidate_index];
            const bool is_invalid_description = !candidate.contract.isValid() || !candidate.method.isValid() ||
                candidate.schema_version == 0U || candidate.schema_hash == 0U;
            if (is_invalid_description)
                return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::INVALID_DESCRIPTION);

            for (const auto& existing : nodes_)
            {
                if (existing.contract != candidate.contract)
                    continue;
                if (existing.schema_version != candidate.schema_version || existing.schema_hash != candidate.schema_hash)
                    return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA);
                if (existing.method == candidate.method)
                    return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::DUPLICATE_METHOD);
            }
            for (std::size_t previous_index{}; previous_index < candidate_index; ++previous_index)
            {
                const auto& previous = contribution.nodes[previous_index];
                if (previous.contract != candidate.contract)
                    continue;
                if (previous.schema_version != candidate.schema_version || previous.schema_hash != candidate.schema_hash)
                    return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA);
                if (previous.method == candidate.method)
                    return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::DUPLICATE_METHOD);
            }
        }

        try
        {
            nodes_.insert(nodes_.end(), contribution.nodes.begin(), contribution.nodes.end());
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::ALLOCATION_FAILURE);
        }
    }
} // namespace lux::flowforge
