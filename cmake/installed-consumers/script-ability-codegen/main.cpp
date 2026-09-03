#include "InventoryAbility.hpp"
#include "InventoryAbility.ability.generated.hpp"

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    struct InventoryProvider final
    {
        std::uint64_t last_item{};
        std::int32_t value{};

        std::int32_t count(std::uint64_t item) noexcept
        {
            last_item = item;
            return value;
        }

        void setCount(std::uint64_t item, std::int32_t count) noexcept
        {
            last_item = item;
            value = count;
        }

        lux::script::ScriptAbilityStartResult countLater(
            std::uint64_t item,
            lux::script::ScriptAbilityCompletion<std::int32_t> completion
        ) noexcept
        {
            last_item = item;
            auto completed = completion.success(value);
            return completed
                ? lux::script::ScriptAbilityStartResult{}
                : lux::script::ScriptAbilityStartResult{
                    lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{91})
                };
        }
    };

    struct InvalidProvider final
    {
        void count(std::uint64_t) noexcept
        {
        }
    };

    struct CompletionState final
    {
        std::int32_t value{};
    };

    lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> completeCount(
        void* state,
        std::int32_t value
    ) noexcept
    {
        static_cast<CompletionState*>(state)->value = value;
        return {};
    }

    lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> failCount(
        void*,
        lux::script::ScriptAbilityOperationError
    ) noexcept
    {
        return {};
    }

    template <class Api>
    concept HasImmediateCountLater = requires(Api api)
    {
        api.countLater(std::uint64_t{});
    };
}

int main()
{
    using Ability = installed_consumer::InventoryAbility;
    using Traits = lux::script::ScriptAbilityTraits<Ability>;
    static_assert(Traits::ProviderConforms<InventoryProvider>);
    static_assert(!Traits::ProviderConforms<InvalidProvider>);
    static_assert(Traits::Description.schema_hash != 0U);
    static_assert(!HasImmediateCountLater<lux::script::ScriptAbilityCpp<Ability>>);

    InventoryProvider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    auto api = lux::script::ScriptAbilityCpp<Ability>::create(binding);
    assert(api);
    api->setCount(17U, 4);
    assert(api->count(17U) == 4);
    assert(provider.last_item == 17U);

    auto starter = lux::script::ScriptAbilityStarter<Ability>::create(binding);
    assert(starter);
    auto state = std::make_shared<CompletionState>();
    auto completion = lux::script::ScriptAbilityCompletion<std::int32_t>::bind(
        std::static_pointer_cast<void>(state),
        &completeCount,
        &failCount
    );
    assert(starter->countLater(18U, std::move(completion)));
    assert(state->value == 4);
    assert(provider.last_item == 18U);

    lux::flowforge::ScriptAbilityNodeCatalog catalog;
    assert(catalog.add(lux::flowforge::makeScriptAbilityCatalogContribution<Ability>()));
    const auto* count_node = catalog.view().find(
        Traits::Description.id,
        lux::script::ScriptApiMethodIdView{"consumer.inventory.count"}
    );
    assert(count_node != nullptr);
    lux::flowforge::ScriptAbilityNode graph_node{*count_node};
    assert(graph_node.contract() == Traits::Description.id);
    assert(graph_node.method() == count_node->method);
    assert(graph_node.parameterPins().size() == 1U);
    assert(graph_node.resultPins().size() == 1U);
    return 0;
}
