#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    using namespace lux::flowforge;
    using namespace lux::script;

    void require(bool value) noexcept
    {
        if (!value)
        {
            std::puts("FAIL: borrowed ability description changed after admission");
            std::exit(42);
        }
    }

    struct Input final
    {
        std::string contract{"vendor.runtime.ability"};
        std::string method{"compute"};
        std::string contract_label{"External runtime ability"};
        std::string method_label{"Compute result"};
        std::string parameter{"argument"};
        std::string parameter_type{"lux.i32"};
        std::string result_type{"lux.i32"};
        std::array<ScriptAbilityParameterDescription, 1> parameters;
        std::array<ScriptAbilityValueDescription, 1> results;
        ScriptAbilityNodeDescription description;

        Input() noexcept
        {
            const auto value = [](const std::string& name) noexcept
            {
                return ScriptAbilityValueDescription{
                    lux::semantic::typeId(name),
                    name,
                    lux::semantic::EValuePass::VALUE,
                    static_cast<std::uint8_t>(lux::semantic::EAbiKind::I32),
                    4,
                    4,
                    EScriptAbilityValueLifetime::OWNED_VALUE
                };
            };
            parameters[0] = {parameter, value(parameter_type)};
            results[0] = value(result_type);
            description = {
                ScriptApiContractIdView{contract},
                ScriptApiMethodIdView{method},
                contract_label,
                method_label,
                7,
                919,
                EScriptAbilityReceiverKind::NONE,
                EScriptApiMethodKind::QUERY,
                parameters,
                results
            };
        }

        void overwrite() noexcept
        {
            for (auto* text :
                 {&contract, &method, &contract_label, &method_label, &parameter, &parameter_type, &result_type})
            {
                std::ranges::fill(*text, '#');
            }
            parameters[0].value.size = 99;
            results[0].alignment = 99;
        }
    };

    void check(const ScriptAbilityNodeDescription& value) noexcept
    {
        require(value.contract.name() == "vendor.runtime.ability");
        require(value.method.name() == "compute");
        require(value.contract_display_name == "External runtime ability");
        require(value.method_display_name == "Compute result");
        require(value.schema_version == 7 && value.schema_hash == 919);
        require(value.parameters.size() == 1 && value.results.size() == 1);
        require(value.parameters[0].name == "argument");
        require(value.parameters[0].value.canonical_name == "lux.i32");
        require(value.parameters[0].value.size == 4);
        require(value.results[0].canonical_name == "lux.i32" && value.results[0].alignment == 4);
    }

    void check(const ScriptAbilityNode& value) noexcept
    {
        require(value.contract().name() == "vendor.runtime.ability");
        require(value.method().name() == "compute");
        require(value.expectedSchemaVersion() == 7 && value.expectedSchemaHash() == 919);
        require(value.parameters().size() == 1 && value.results().size() == 1);
        require(value.parameters()[0].name == "argument");
        require(value.parameters()[0].value.canonical_name == "lux.i32");
        require(value.results()[0].canonical_name == "lux.i32");
        require(value.parameters()[0].value.size == 4 && value.results()[0].alignment == 4);
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace lux::flowforge;
    const std::string mode = argc > 1 ? argv[1] : "catalog";
    if (mode == "catalog")
    {
        ScriptAbilityNodeCatalog catalog;
        {
            Input input;
            require(catalog.add({{&input.description, 1}}).has_value());
            input.overwrite();
            check(catalog.view().nodes().front());
        }
        check(catalog.view().nodes().front());
        auto before = catalog.view();
        Input duplicate;
        const auto failure = catalog.add({{&duplicate.description, 1}});
        require(!failure && failure.error() == EScriptAbilityNodeCatalogError::DUPLICATE_METHOD);
        require(before.nodes().data() == catalog.view().nodes().data());
        check(before.nodes().front());

        auto changed = duplicate.description;
        changed.method = ScriptApiMethodIdView{"another"};
        auto conflict = changed;
        conflict.schema_hash = 920;
        const auto mixed = std::array{changed, conflict};
        const auto independently_rejected = validateScriptAbilityNodes(mixed);
        require(
            !independently_rejected &&
            independently_rejected.error() == EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA
        );
        const auto rejected = catalog.add({mixed});
        require(!rejected && rejected.error() == EScriptAbilityNodeCatalogError::CONFLICTING_CONTRACT_SCHEMA);
        require(catalog.view().nodes().size() == 1 && before.nodes().data() == catalog.view().nodes().data());
        require(!catalog.view().find(duplicate.description.contract, changed.method));
        changed.schema_version = 0;
        const auto invalid = catalog.add({{&changed, 1}});
        require(!invalid && invalid.error() == EScriptAbilityNodeCatalogError::INVALID_DESCRIPTION);

        for (int index{}; index != 128; ++index)
        {
            std::string name = "external.method.with.long.owned.name." + std::to_string(index);
            auto addition = duplicate.description;
            addition.method = ScriptApiMethodIdView{name};
            require(catalog.add({{&addition, 1}}).has_value());
        }
        require(catalog.view().nodes().size() == 129);
        check(catalog.view().nodes().front());
        for (int index{}; index != 128; ++index)
        {
            const std::string name = "external.method.with.long.owned.name." + std::to_string(index);
            const auto* found = catalog.view().find(duplicate.description.contract, ScriptApiMethodIdView{name});
            require(found != nullptr && found->method.name() == name && found->parameters[0].name == "argument");
        }
    }
    else if (mode == "node")
    {
        std::unique_ptr<ScriptAbilityNode> node;
        {
            Input input;
            node = std::make_unique<ScriptAbilityNode>(input.description);
            input.overwrite();
            check(*node);
        }
        check(*node);
        {
            ScriptAbilityNodeCatalog catalog;
            Input input;
            require(catalog.add({{&input.description, 1}}).has_value());
            node = std::make_unique<ScriptAbilityNode>(catalog.view().nodes().front());
        }
        check(*node);
    }
    else
    {
        return 2;
    }
    std::printf("PASS: real Flow %s owns complete ability description\n", mode.c_str());
}
