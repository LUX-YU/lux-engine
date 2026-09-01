#include <lux/engine/system/SystemTypeDescription.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <array>
#include <cassert>

int main()
{
    using namespace lux;

    static constexpr std::array<std::string_view, 1U> capabilities{"lux.test"};
    constexpr system::SystemTypeDescription description{
        .canonical_name = "lux.test.system",
        .version = 1U,
        .capabilities = capabilities,
        .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER
    };
    static_assert(system::validSystemTypeDescription(description));

    const std::array instances{
        system::SystemInstanceId{30U},
        system::SystemInstanceId{10U},
        system::SystemInstanceId{20U}
    };
    const std::array edges{
        system::detail::SystemDependencyOrdinalEdge{1U, 0U},
        system::detail::SystemDependencyOrdinalEdge{2U, 0U}
    };
    const auto order = system::detail::deterministicSystemOrder(instances, edges);
    assert(order);
    assert((*order == std::vector<std::size_t>{1U, 2U, 0U}));

    const std::array cycle{
        system::detail::SystemDependencyOrdinalEdge{0U, 1U},
        system::detail::SystemDependencyOrdinalEdge{1U, 0U}
    };
    const auto cycle_result = system::detail::deterministicSystemOrder(instances, cycle);
    assert(!cycle_result && cycle_result.error() == system::detail::ESystemDependencyOrderError::CYCLE);
    return 0;
}
