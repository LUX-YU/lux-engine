#pragma once

#include <optional>
#include <string_view>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/Signal.hpp>

namespace lux::object::reflection
{
    struct SignalView final
    {
        const SignalRuntime* signal{nullptr};
        const lux::meta::RefStaticField* field{nullptr};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return signal != nullptr && field != nullptr;
        }
    };

    [[nodiscard]] LUX_CORE_PUBLIC SignalView findDeclaredSignal(
        const lux::meta::RefClass& object_class,
        std::string_view name
    ) noexcept;

    [[nodiscard]] LUX_CORE_PUBLIC SignalView findSignal(
        const lux::meta::ReflectionRegistry& registry,
        const lux::meta::RefClass& object_class,
        std::string_view name
    ) noexcept;

    enum class EDynamicObserveError
    {
        INVALID_SIGNAL,
        METHOD_NOT_CONNECTABLE,
        METHOD_MUST_BE_INSTANCE,
        RECEIVER_TYPE_MISMATCH,
        RETURN_TYPE_MISMATCH,
        PARAMETER_COUNT_MISMATCH,
        PARAMETER_TYPE_MISMATCH,
        PAYLOAD_NOT_QUEUEABLE,
        CONNECTION_REJECTED
    };

    [[nodiscard]] LUX_CORE_PUBLIC lux::cxx::expected<Connection, EDynamicObserveError>
    observe(
        lux::object::LuxObject&     sender,
        SignalView                  signal,
        lux::object::LuxObject&     receiver,
        const lux::meta::RefMethod& method,
        lux::object::EDelivery      delivery = lux::object::EDelivery::AUTO
    );
} // namespace lux::object::reflection
