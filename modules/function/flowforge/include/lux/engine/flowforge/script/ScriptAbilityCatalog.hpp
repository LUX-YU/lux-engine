#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lux::flowforge
{
    namespace detail
    {
        class ScriptAbilityNodeStorage;
    }

    struct ScriptAbilityNodeDescription final
    {
        lux::script::ScriptApiContractIdView contract;
        lux::script::ScriptApiMethodIdView method;
        std::string_view contract_display_name;
        std::string_view method_display_name;
        std::uint32_t schema_version{1U};
        std::uint64_t schema_hash{};
        lux::script::EScriptAbilityReceiverKind receiver{lux::script::EScriptAbilityReceiverKind::NONE};
        lux::script::EScriptApiMethodKind kind{lux::script::EScriptApiMethodKind::QUERY};
        std::span<const lux::script::ScriptAbilityParameterDescription> parameters;
        std::span<const lux::script::ScriptAbilityValueDescription> results;
    };

    template <class Ability> struct TScriptAbilityCatalog final
    {
        using Traits = lux::script::TScriptAbilityTraits<Ability>;

        inline static constexpr auto Nodes = []() consteval
        {
            std::array<ScriptAbilityNodeDescription, Traits::Methods.size()> result{};
            for (std::size_t index{}; index < result.size(); ++index)
            {
                const auto& method = Traits::Methods[index];
                result[index] = {
                    Traits::Description.id,
                    method.id,
                    Traits::Description.display_name,
                    method.display_name,
                    Traits::Description.schema_version,
                    Traits::Description.schema_hash,
                    Traits::Description.receiver,
                    method.kind,
                    method.parameters,
                    method.results
                };
            }
            return result;
        }();
    };

    template <class Ability>
    [[nodiscard]] constexpr std::span<const ScriptAbilityNodeDescription> scriptAbilityNodes() noexcept
    {
        return TScriptAbilityCatalog<Ability>::Nodes;
    }

    struct ScriptAbilityCatalogContribution final
    {
        std::span<const ScriptAbilityNodeDescription> nodes;
    };

    template <class Ability>
    [[nodiscard]] constexpr ScriptAbilityCatalogContribution makeScriptAbilityCatalogContribution() noexcept
    {
        return {scriptAbilityNodes<Ability>()};
    }

    enum class EScriptAbilityNodeCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        DUPLICATE_METHOD,
        CONFLICTING_CONTRACT_SCHEMA,
    };

    // Pure validation used by source decoding as well as directory admission; no retained input or allocation.
    [[nodiscard]] lux::cxx::expected<void, EScriptAbilityNodeCatalogError> validateScriptAbilityNodes(
        std::span<const ScriptAbilityNodeDescription> nodes
    ) noexcept;

    class ScriptAbilityNodeCatalogView final
    {
    public:
        ScriptAbilityNodeCatalogView() = default;

        explicit ScriptAbilityNodeCatalogView(std::span<const ScriptAbilityNodeDescription> nodes) noexcept
            : nodes_(nodes)
        {
        }

        [[nodiscard]] std::span<const ScriptAbilityNodeDescription> nodes() const noexcept
        {
            return nodes_;
        }

        [[nodiscard]] const ScriptAbilityNodeDescription* find(
            lux::script::ScriptApiContractIdView contract,
            lux::script::ScriptApiMethodIdView method
        ) const noexcept;

    private:
        std::span<const ScriptAbilityNodeDescription> nodes_;
    };

    class ScriptAbilityNodeCatalog final
    {
    public:
        ScriptAbilityNodeCatalog() noexcept;
        ~ScriptAbilityNodeCatalog();

        ScriptAbilityNodeCatalog(const ScriptAbilityNodeCatalog&) = delete;
        ScriptAbilityNodeCatalog& operator=(const ScriptAbilityNodeCatalog&) = delete;
        ScriptAbilityNodeCatalog(ScriptAbilityNodeCatalog&&) = delete;
        ScriptAbilityNodeCatalog& operator=(ScriptAbilityNodeCatalog&&) = delete;

        // Copies the entire description. The contributor may release its data/code after add returns.
        // Rejection leaves the catalog and all outstanding borrowed views unchanged.
        [[nodiscard]] lux::cxx::expected<void, EScriptAbilityNodeCatalogError> add(
            ScriptAbilityCatalogContribution contribution
        ) noexcept;

        // Borrowed until the next successful add or catalog destruction; not a publication/worker snapshot.
        [[nodiscard]] ScriptAbilityNodeCatalogView view() const noexcept
        {
            return ScriptAbilityNodeCatalogView(nodes_);
        }

    private:
        std::vector<std::unique_ptr<const detail::ScriptAbilityNodeStorage>> storage_;
        std::vector<ScriptAbilityNodeDescription> nodes_;
    };
} // namespace lux::flowforge
