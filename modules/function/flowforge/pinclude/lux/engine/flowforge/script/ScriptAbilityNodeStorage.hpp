#pragma once

#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>

#include <string>

namespace lux::flowforge::detail
{
    // Immutable metadata only: no plugin callback or borrowed backing survives construction.
    class ScriptAbilityNodeStorage final
    {
    public:
        explicit ScriptAbilityNodeStorage(const ScriptAbilityNodeDescription& source) noexcept
            : parameters_(source.parameters.begin(), source.parameters.end()),
              results_(source.results.begin(), source.results.end()), description_(source)
        {
            strings_.reserve(4 + parameters_.size() * 2 + results_.size());
            description_.contract = lux::script::ScriptApiContractIdView{store(source.contract.name())};
            description_.method = lux::script::ScriptApiMethodIdView{store(source.method.name())};
            description_.contract_display_name = store(source.contract_display_name);
            description_.method_display_name = store(source.method_display_name);
            for (auto& parameter : parameters_)
            {
                parameter.name = store(parameter.name);
                parameter.value.canonical_name = store(parameter.value.canonical_name);
            }
            for (auto& result : results_)
            {
                result.canonical_name = store(result.canonical_name);
            }
            description_.parameters = parameters_;
            description_.results = results_;
        }

        ScriptAbilityNodeStorage(const ScriptAbilityNodeStorage&) = delete;
        ScriptAbilityNodeStorage& operator=(const ScriptAbilityNodeStorage&) = delete;
        ScriptAbilityNodeStorage(ScriptAbilityNodeStorage&&) = delete;
        ScriptAbilityNodeStorage& operator=(ScriptAbilityNodeStorage&&) = delete;

        [[nodiscard]] const ScriptAbilityNodeDescription& description() const noexcept
        {
            return description_;
        }

        [[nodiscard]] std::size_t bytes() const noexcept
        {
            std::size_t result = sizeof(*this) + strings_.capacity() * sizeof(std::string) +
                                 parameters_.capacity() * sizeof(lux::script::ScriptAbilityParameterDescription) +
                                 results_.capacity() * sizeof(lux::script::ScriptAbilityValueDescription);
            for (const auto& text : strings_)
            {
                // Conservative accounting includes inline string capacity, matching the original node budget.
                result += text.capacity() + 1;
            }
            return result;
        }

    private:
        [[nodiscard]] std::string_view store(std::string_view value) noexcept
        {
            return strings_.emplace_back(value);
        }

        std::vector<std::string> strings_;
        std::vector<lux::script::ScriptAbilityParameterDescription> parameters_;
        std::vector<lux::script::ScriptAbilityValueDescription> results_;
        ScriptAbilityNodeDescription description_;
    };
} // namespace lux::flowforge::detail
