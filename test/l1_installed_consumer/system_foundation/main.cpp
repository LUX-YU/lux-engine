#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/system/SystemTypeDescription.hpp>
#include <lux/engine/system/SystemTypeId.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

int main()
{
    static constexpr std::array<std::string_view, 1U> capabilities{"lux.consumer"};
    constexpr lux::system::SystemTypeDescription description{
        .canonical_name = "lux.consumer.system",
        .version = 1U,
        .capabilities = capabilities
    };
    if (!lux::system::validSystemTypeDescription(description))
    {
        return 1;
    }
    const auto type = lux::system::systemTypeId(description.canonical_name);
    if (!type.valid())
    {
        return 2;
    }
    const std::array instances{
        lux::system::SystemInstanceId{2U},
        lux::system::SystemInstanceId{1U}
    };
    const auto order = lux::system::detail::deterministicSystemOrder(instances, {});
    return order && (*order == std::vector<std::size_t>{1U, 0U}) ? 0 : 3;
}
