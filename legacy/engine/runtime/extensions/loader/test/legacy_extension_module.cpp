#include <cstddef>

extern "C" __declspec(dllexport)
const void* legacyGetDescriptor() noexcept
{
    return nullptr;
}
