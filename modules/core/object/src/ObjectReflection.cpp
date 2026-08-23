#include <lux/engine/object/ObjectReflection.hpp>

#include <array>
#include <utility>

namespace lux::object::reflection
{
    lux::cxx::expected<Connection, EDynamicObserveError> observe(
        lux::object::LuxObject& sender,
        SignalView signal,
        lux::object::LuxObject& receiver,
        const lux::meta::RefMethod& method,
        lux::object::EDelivery delivery
    )
    {
        using lux::meta::EBaseType;
        using lux::meta::ETypeQual;

        if (!signal || !signal.signal || !signal.field)
            return lux::cxx::unexpected(EDynamicObserveError::INVALID_SIGNAL);
        if (!method.annotations().has("connectable"))
            return lux::cxx::unexpected(EDynamicObserveError::METHOD_NOT_CONNECTABLE);
        if (method.is_static || !method.invokable.invoker)
            return lux::cxx::unexpected(EDynamicObserveError::METHOD_MUST_BE_INSTANCE);
        if (!method.owner_class || !receiver.isObjectType(lux::cxx::TypeToken{
                                       method.owner_class->type.hash,
                                       method.owner_class->full_name
                                   }))
        {
            return lux::cxx::unexpected(EDynamicObserveError::RECEIVER_TYPE_MISMATCH);
        }

        const auto& invokable = method.invokable;
        if (static_cast<EBaseType>(invokable.return_type.qtype.base) !=
                EBaseType::Void ||
            static_cast<ETypeQual>(invokable.return_type.qtype.qual) !=
                ETypeQual::Value)
        {
            return lux::cxx::unexpected(EDynamicObserveError::RETURN_TYPE_MISMATCH);
        }
        const auto expected_parameter_count =
            signal.signal->has_payload ? std::size_t{1} : std::size_t{0};
        if (invokable.parameters.size() != expected_parameter_count)
            return lux::cxx::unexpected(EDynamicObserveError::PARAMETER_COUNT_MISMATCH);

        if (signal.signal->has_payload)
        {
            const auto& parameter = invokable.parameters.front();
            if (static_cast<ETypeQual>(parameter.type.qtype.qual) !=
                    ETypeQual::LRefToConst ||
                parameter.value_type_hash != signal.signal->payload.hash() ||
                parameter.value_type_name != signal.signal->payload.name())
            {
                return lux::cxx::unexpected(
                    EDynamicObserveError::PARAMETER_TYPE_MISMATCH
                );
            }
        }

        struct DynamicInvoke final
        {
            decltype(invokable.invoker) invoker{nullptr};
            bool has_payload{false};
        };
        auto context = std::make_shared<DynamicInvoke>(
            DynamicInvoke{invokable.invoker, signal.signal->has_payload}
        );
        auto connected = detail::observeDynamicErased(
            sender,
            *signal.signal,
            receiver,
            [](LuxObject* object, const void* payload, void* raw_context)
            {
                auto& invoke = *static_cast<DynamicInvoke*>(raw_context);
                if (invoke.has_payload)
                {
                    std::array<void*, 1> arguments{const_cast<void*>(payload)};
                    invoke.invoker(object, arguments.data(), nullptr);
                }
                else
                {
                    invoke.invoker(object, nullptr, nullptr);
                }
            },
            std::move(context),
            delivery
        );
        if (!connected)
        {
            if (connected.error() == EObserveError::PAYLOAD_NOT_QUEUEABLE)
            {
                return lux::cxx::unexpected(
                    EDynamicObserveError::PAYLOAD_NOT_QUEUEABLE
                );
            }
            return lux::cxx::unexpected(EDynamicObserveError::CONNECTION_REJECTED);
        }
        return *connected;
    }
} // namespace lux::object::reflection
