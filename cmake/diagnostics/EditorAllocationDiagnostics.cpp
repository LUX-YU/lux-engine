// Diagnostic DLL-local replacement allocation functions. Not part of an installed SDK.
// Linking this source into the tested DLL intercepts its real C++ allocations, including STL growth.
// EXE-local allocation counts cannot observe those allocations on Windows.
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#define LUX_DIAGNOSTIC_EXPORT __declspec(dllexport)
#else
#define LUX_DIAGNOSTIC_EXPORT __attribute__((visibility("default")))
#endif
namespace
{
    thread_local std::size_t remaining = (std::numeric_limits<std::size_t>::max)();
    thread_local std::size_t attempts{};
    void checkAllocation()
    {
        if (remaining == (std::numeric_limits<std::size_t>::max)())
            return;
        ++attempts;
        if (!remaining)
        {
            remaining = (std::numeric_limits<std::size_t>::max)();
            throw std::bad_alloc{};
        }
        --remaining;
    }
    void *allocate(std::size_t bytes)
    {
        checkAllocation();
        if (auto *result = std::malloc(bytes ? bytes : 1))
            return result;
        throw std::bad_alloc{};
    }
    void *allocateAligned(std::size_t bytes, std::size_t alignment)
    {
        checkAllocation();
#if defined(_WIN32)
        if (auto *result = _aligned_malloc(bytes ? bytes : 1, alignment))
            return result;
#else
        void *result{};
        if (!posix_memalign(&result, alignment, bytes ? bytes : 1))
            return result;
#endif
        throw std::bad_alloc{};
    }
    void freeAligned(void *address) noexcept
    {
#if defined(_WIN32)
        _aligned_free(address);
#else
        std::free(address);
#endif
    }
}
extern "C" LUX_DIAGNOSTIC_EXPORT void LUX_ALLOCATION_ARM(std::size_t successful_allocations) noexcept
{
    remaining = successful_allocations;
    attempts = 0;
}
extern "C" LUX_DIAGNOSTIC_EXPORT std::size_t LUX_ALLOCATION_DISARM() noexcept
{
    remaining = (std::numeric_limits<std::size_t>::max)();
    return attempts;
}
void *operator new(std::size_t bytes) { return allocate(bytes); }
void *operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void *address) noexcept { std::free(address); }
void operator delete[](void *address) noexcept { std::free(address); }
void operator delete(void *address, std::size_t) noexcept { std::free(address); }
void operator delete[](void *address, std::size_t) noexcept { std::free(address); }
void *operator new(std::size_t bytes, std::align_val_t alignment)
{ return allocateAligned(bytes, static_cast<std::size_t>(alignment)); }
void *operator new[](std::size_t bytes, std::align_val_t alignment)
{ return allocateAligned(bytes, static_cast<std::size_t>(alignment)); }
void operator delete(void *address, std::align_val_t) noexcept { freeAligned(address); }
void operator delete[](void *address, std::align_val_t) noexcept { freeAligned(address); }
void operator delete(void *address, std::size_t, std::align_val_t) noexcept { freeAligned(address); }
void operator delete[](void *address, std::size_t, std::align_val_t) noexcept { freeAligned(address); }
