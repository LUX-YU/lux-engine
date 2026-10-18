#pragma once

#include <cstddef>
#include <lux/cxx/compile_time/TypeToken.hpp>

namespace lux::render
{
    // Controlled C++ object contract. This does not describe a wire or Shader layout.
    // The token name borrows static type metadata from the defining module.
    struct CppType final
    {
        cxx::TypeToken token;
        std::size_t size = 0;
        std::size_t alignment = 0;

        constexpr bool operator==(const CppType&) const noexcept = default;
    };

    template <class T> [[nodiscard]] constexpr CppType cppType() noexcept
    {
        return {cxx::typeToken<T>(), sizeof(T), alignof(T)};
    }
} // namespace lux::render
