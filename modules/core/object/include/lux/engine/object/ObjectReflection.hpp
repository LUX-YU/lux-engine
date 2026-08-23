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
        const SignalHeader* signal{nullptr};
        const lux::meta::RefStaticField* field{nullptr};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return signal != nullptr && field != nullptr;
        }
    };

    [[nodiscard]] inline SignalView findSignal(
        const lux::meta::RefClass& object_class,
        std::string_view name
    ) noexcept
    {
        for (const auto& field : object_class.static_fields)
        {
            if (field.name != name || !field.annotations().has("signal")) continue;
            const auto* signal = static_cast<const SignalHeader*>(field.address);
            if (!signal || signal->key.name != field.name) return {};
            return {signal, &field};
        }
        return {};
    }

    enum class EDynamicObserveError
    {
        INVALID_SIGNAL,
        METHOD_NOT_CONNECTABLE,
        METHOD_MUST_BE_INSTANCE,
        RECEIVER_TYPE_MISMATCH,
        RETURN_TYPE_MISMATCH,
        PARAMETER_COUNT_MISMATCH,
        PARAMETER_TYPE_MISMATCH,
        CONNECTION_REJECTED
    };

    [[nodiscard]] LUX_CORE_PUBLIC lux::cxx::expected<
        Connection,
        EDynamicObserveError>
    observe(
        lux::object::LuxObject& sender,
        SignalView signal,
        lux::object::LuxObject& receiver,
        const lux::meta::RefMethod& method,
        lux::object::EDelivery delivery = lux::object::EDelivery::AUTO
    );
}
