#pragma once

#include <lux/engine/flowforge/graph/NodeBase.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>

#include <memory>
#include <vector>

namespace lux::flowforge
{
    class ScriptAbilityNode final : public ExecIntermediateNode
    {
    public:
        explicit ScriptAbilityNode(const ScriptAbilityNodeDescription& description);
        ~ScriptAbilityNode() override;

        [[nodiscard]] const ScriptAbilityNode* scriptAbility() const noexcept override
        {
            return this;
        }

        [[nodiscard]] lux::script::ScriptApiContractIdView contract() const noexcept;
        [[nodiscard]] lux::script::ScriptApiMethodIdView method() const noexcept;
        [[nodiscard]] std::uint32_t expectedSchemaVersion() const noexcept;
        [[nodiscard]] std::uint64_t expectedSchemaHash() const noexcept;
        [[nodiscard]] std::size_t descriptionBytes() const noexcept;
        [[nodiscard]] lux::script::EScriptApiMethodKind methodKind() const noexcept;
        [[nodiscard]] lux::script::EScriptAbilityReceiverKind receiverKind() const noexcept;
        [[nodiscard]] std::span<const lux::script::ScriptAbilityParameterDescription> parameters() const noexcept;
        [[nodiscard]] std::span<const lux::script::ScriptAbilityValueDescription> results() const noexcept;

        [[nodiscard]] const std::vector<std::unique_ptr<DataInPin>>& parameterPins() const noexcept
        {
            return parameter_pins_;
        }

        [[nodiscard]] const std::vector<std::unique_ptr<DataOutPin>>& resultPins() const noexcept
        {
            return result_pins_;
        }

    private:
        struct TypeStorage;

        [[nodiscard]] const lux::meta::RefType* storeType(const lux::script::ScriptAbilityValueDescription& description
        );

        std::unique_ptr<const detail::ScriptAbilityNodeStorage> description_;
        std::vector<std::unique_ptr<TypeStorage>> types_;
        std::vector<std::unique_ptr<DataInPin>> parameter_pins_;
        std::vector<std::unique_ptr<DataOutPin>> result_pins_;
    };
} // namespace lux::flowforge
