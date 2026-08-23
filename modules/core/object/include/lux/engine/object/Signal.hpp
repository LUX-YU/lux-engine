#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/core/StableNameId.hpp>

namespace lux::object
{
    struct NoSignalPayload final {};

    struct SignalKey final
    {
        lux::cxx::TypeToken owner;
        lux::cxx::TypeToken payload;
        std::uint64_t name_hash{0};
        std::string_view name;

        [[nodiscard]] constexpr bool operator==(const SignalKey&) const noexcept = default;
    };

    struct SignalHeader final
    {
        SignalKey key;
    };

    static_assert(std::is_standard_layout_v<SignalHeader>);

    template<typename Owner, typename Payload = NoSignalPayload>
    class Signal final
    {
      public:
        using owner_type = Owner;
        using payload_type = Payload;

        constexpr explicit Signal(std::string_view name) noexcept
            : header_{SignalKey{
                lux::cxx::typeToken<Owner>(),
                lux::cxx::typeToken<Payload>(),
                lux::cxx::Fnv1a64::hash(name),
                name
            }}
        {
        }

        [[nodiscard]] constexpr const SignalHeader& header() const noexcept
        {
            return header_;
        }

        [[nodiscard]] constexpr std::string_view name() const noexcept
        {
            return header_.key.name;
        }

        // Public for reflection's address-only dynamic view. This is immutable
        // descriptor data, not mutable object state.
        SignalHeader header_;
    };

    static_assert(std::is_standard_layout_v<Signal<int, int>>);
}
