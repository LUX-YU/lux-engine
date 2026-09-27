#pragma once
#include <cstdlib>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::ui::detail
{
    [[noreturn]] inline void failContract() noexcept
    {
#if defined(_MSC_VER)
        __fastfail(7u);
#else
        std::abort();
#endif
    }
}
