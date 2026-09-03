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
        ScriptAbilityNode(std::uint64_t id, const ScriptAbilityNodeDescription& description);
        explicit ScriptAbilityNode(const ScriptAbilityNodeDescription& description);
        ~ScriptAbilityNode() override;

        [[nodiscard]] lux::script::ScriptApiContractIdView contract() const noexcept
        {
            return lux::script::ScriptApiContractIdView{contract_.name()};
        }
        [[nodiscard]] lux::script::ScriptApiMethodIdView method() const noexcept
        {
            return lux::script::ScriptApiMethodIdView{method_.name()};
        }
        [[nodiscard]] std::uint32_t expectedSchemaVersion() const noexcept { return schema_version_; }
        [[nodiscard]] std::uint64_t expectedSchemaHash() const noexcept { return schema_hash_; }
        [[nodiscard]] lux::script::EScriptApiMethodKind methodKind() const noexcept { return kind_; }
        [[nodiscard]] lux::script::EScriptAbilityReceiverKind receiverKind() const noexcept { return receiver_; }
        [[nodiscard]] std::span<const lux::script::ScriptAbilityParameterDescription> parameters() const noexcept
        {
            return parameters_;
        }
        [[nodiscard]] std::span<const lux::script::ScriptAbilityValueDescription> results() const noexcept
        {
            return results_;
        }
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

        [[nodiscard]] const lux::meta::RefType* storeType(
            const lux::script::ScriptAbilityValueDescription& description
        );

        lux::script::ScriptApiContractId contract_;
        lux::script::ScriptApiMethodId method_;
        std::uint32_t schema_version_{1U};
        std::uint64_t schema_hash_{};
        lux::script::EScriptApiMethodKind kind_{lux::script::EScriptApiMethodKind::QUERY};
        lux::script::EScriptAbilityReceiverKind receiver_{lux::script::EScriptAbilityReceiverKind::NONE};
        std::vector<lux::script::ScriptAbilityParameterDescription> parameters_;
        std::vector<lux::script::ScriptAbilityValueDescription> results_;
        std::vector<std::unique_ptr<TypeStorage>> types_;
        std::vector<std::unique_ptr<DataInPin>> parameter_pins_;
        std::vector<std::unique_ptr<DataOutPin>> result_pins_;
    };
} // namespace lux::flowforge
