#pragma once
/**
 * @file ScriptValue.hpp
 * @brief C++-friendly views over @ref lux_script_value_slot for module authors.
 */

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <lux/engine/function/script/abi/lux_script_abi.h>

namespace lux::script
{
    using ValueKind = ::lux_script_value_kind;
    using ScriptTypeId = std::uint64_t;

    /** Read-only view of an argument slot. */
    struct ConstValueView
    {
        ValueKind     kind;
        ScriptTypeId  type_id;
        const void*   data;
        std::uint32_t size;
    };

    /** Mutable view of a return slot. */
    struct ValueView
    {
        ValueKind     kind;
        ScriptTypeId  type_id;
        void*         data;
        std::uint32_t size;
    };

    inline ConstValueView make_const_view(const lux_script_value_slot& s) noexcept
    {
        return ConstValueView{
            static_cast<ValueKind>(s.kind),
            s.type_id,
            s.data,
            s.size};
    }

    inline ValueView make_view(lux_script_value_slot& s) noexcept
    {
        return ValueView{
            static_cast<ValueKind>(s.kind),
            s.type_id,
            s.data,
            s.size};
    }

    /** Best-effort typed read; caller is responsible for kind/size matching. */
    template <typename T>
    inline T read(const ConstValueView& v) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "lux::script::read<T> requires trivially copyable T");
        T out{};
        if (v.data && v.size >= sizeof(T))
        {
            std::memcpy(&out, v.data, sizeof(T));
        }
        return out;
    }
}
