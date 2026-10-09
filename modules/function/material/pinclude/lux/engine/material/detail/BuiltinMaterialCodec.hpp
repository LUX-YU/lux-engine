#pragma once

#include <lux/engine/material/BuiltinMaterialNodes.hpp>

namespace lux::material::detail
{
    // Source-v1 payload tables and registered source-v2 payload bytes use these same codecs.
    template <class T> [[nodiscard]] MaterialNodeResult<std::string> encodeBuiltinPayload(const T&) noexcept;
    template <class T> [[nodiscard]] MaterialNodeResult<T> decodeBuiltinPayload(std::string_view) noexcept;

    template <class T, auto Clone> void installBuiltinCodec(MaterialNodeRegistration& registration) noexcept
    {
        registration.encode = [](const MaterialNodePayload& value) noexcept
        { return encodeBuiltinPayload(*value.get<T>()); };
        registration.decode = [](std::string_view bytes,
                                 const object::CodeLease& code) noexcept -> MaterialNodeResult<MaterialNodePayload>
        {
            auto value = decodeBuiltinPayload<T>(bytes);
            if (!value)
            {
                return cxx::unexpected(std::move(value.error()));
            }
            return MaterialNodePayload::make<T, Clone>(code, std::move(*value));
        };
    }
} // namespace lux::material::detail
