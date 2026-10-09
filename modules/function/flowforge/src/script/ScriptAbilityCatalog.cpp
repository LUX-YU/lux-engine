#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNodeStorage.hpp>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] lux::cxx::expected<void, EScriptAbilityNodeCatalogError> validateNodes(
            std::span<const ScriptAbilityNodeDescription> existing_nodes,
            std::span<const ScriptAbilityNodeDescription> candidates
        ) noexcept
        {
            for (std::size_t candidate_index{}; candidate_index < candidates.size(); ++candidate_index)
            {
                const auto& candidate = candidates[candidate_index];
                const bool is_invalid_description = !candidate.contract.isValid() || !candidate.method.isValid() ||
                                                    candidate.schema_version == 0U || candidate.schema_hash == 0U;
                if (is_invalid_description)
                {
                    return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::INVALID_DESCRIPTION);
                }

                for (const auto& existing : existing_nodes)
                {
                    if (existing.contract != candidate.contract)
                    {
                        continue;
                    }
                    const bool is_schema_mismatch = existing.schema_version != candidate.schema_version ||
                                                    existing.schema_hash != candidate.schema_hash;
                    if (is_schema_mismatch)
                    {
                        return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA);
                    }
                    if (existing.method == candidate.method)
                    {
                        return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::DUPLICATE_METHOD);
                    }
                }
                for (std::size_t previous_index{}; previous_index < candidate_index; ++previous_index)
                {
                    const auto& previous = candidates[previous_index];
                    if (previous.contract != candidate.contract)
                    {
                        continue;
                    }
                    const bool is_schema_mismatch = previous.schema_version != candidate.schema_version ||
                                                    previous.schema_hash != candidate.schema_hash;
                    if (is_schema_mismatch)
                    {
                        return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA);
                    }
                    if (previous.method == candidate.method)
                    {
                        return lux::cxx::unexpected(EScriptAbilityNodeCatalogError::DUPLICATE_METHOD);
                    }
                }
            }
            return {};
        }
    } // namespace

    lux::cxx::expected<void, EScriptAbilityNodeCatalogError> validateScriptAbilityNodes(
        std::span<const ScriptAbilityNodeDescription> nodes
    ) noexcept
    {
        return validateNodes({}, nodes);
    }

    ScriptAbilityNodeCatalog::ScriptAbilityNodeCatalog() noexcept = default;

    ScriptAbilityNodeCatalog::~ScriptAbilityNodeCatalog() = default;

    const ScriptAbilityNodeDescription* ScriptAbilityNodeCatalogView::find(
        lux::script::ScriptApiContractIdView contract,
        lux::script::ScriptApiMethodIdView method
    ) const noexcept
    {
        for (const auto& node : nodes_)
        {
            if (node.contract == contract && node.method == method)
            {
                return &node;
            }
        }
        return nullptr;
    }

    lux::cxx::expected<void, EScriptAbilityNodeCatalogError> ScriptAbilityNodeCatalog::add(
        ScriptAbilityCatalogContribution contribution
    ) noexcept
    {
        if (auto valid = validateNodes(nodes_, contribution.nodes); !valid)
        {
            return valid;
        }

        // Own candidates before reserve: the input is allowed to borrow this catalog's current view.
        std::vector<std::unique_ptr<const detail::ScriptAbilityNodeStorage>> candidates;
        candidates.reserve(contribution.nodes.size());
        for (const auto& description : contribution.nodes)
        {
            candidates.push_back(std::make_unique<detail::ScriptAbilityNodeStorage>(description));
        }
        nodes_.reserve(nodes_.size() + candidates.size());
        storage_.reserve(storage_.size() + candidates.size());
        for (auto& candidate : candidates)
        {
            nodes_.push_back(candidate->description());
            storage_.push_back(std::move(candidate));
        }
        return {};
    }
} // namespace lux::flowforge
