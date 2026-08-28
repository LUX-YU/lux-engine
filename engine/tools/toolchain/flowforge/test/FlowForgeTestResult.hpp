#pragma once

#include <lux/engine/flowforge/compiler/IR.hpp>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace lux::flowforge::test
{
    template<class T>
    T require(FlowForgeResult<T> result, const char* operation)
    {
        if (!result)
        {
            std::fprintf(
                stderr,
                "%s failed: %s\n",
                operation,
                result.error().message.c_str()
            );
            std::abort();
        }
        return std::move(result.value());
    }

    inline void require(FlowForgeResult<void> result, const char* operation)
    {
        if (!result)
        {
            std::fprintf(
                stderr,
                "%s failed: %s\n",
                operation,
                result.error().message.c_str()
            );
            std::abort();
        }
    }
}
