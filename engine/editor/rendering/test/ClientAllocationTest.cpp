#include <cstddef>
#include <cstdio>
extern "C" __declspec(dllimport) unsigned lux_er1_client_allocation_tests(std::size_t *) noexcept;
int main()
{
    std::size_t failures{};
    const auto result = lux_er1_client_allocation_tests(&failures);
    std::printf("client DLL allocation result=%u actual_failures=%zu\n", result, failures);
    return static_cast<int>(result);
}
