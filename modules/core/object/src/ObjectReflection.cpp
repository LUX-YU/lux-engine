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
            return lux::cxx::unexpected(
                EDynamicObserveError::INVALID_SIGNAL
            );
        if (!method.annotations().has("connectable"))
            return lux::cxx::unexpected(
                EDynamicObserveError::METHOD_NOT_CONNECTABLE
            );
        if (method.is_static || !method.invokable.invoker)
            return lux::cxx::unexpected(
                EDynamicObserveError::METHOD_MUST_BE_INSTANCE
            );
        if (!method.owner_class || !receiver.isObjectType(
            lux::cxx::TypeToken{
                method.owner_class->type.hash,
                method.owner_class->full_name
            }
        ))
        {
            return lux::cxx::unexpected(
                EDynamicObserveError::RECEIVER_TYPE_MISMATCH
            );
        }

        const auto& invokable = method.invokable;
        if (static_cast<EBaseType>(invokable.return_type.qtype.base)
                != EBaseType::Void
            || static_cast<ETypeQual>(invokable.return_type.qtype.qual)
                != ETypeQual::Value)
        {
            return lux::cxx::unexpected(
                EDynamicObserveError::RETURN_TYPE_MISMATCH
            );
        }
        if (invokable.parameters.size() != 1)
            return lux::cxx::unexpected(
                EDynamicObserveError::PARAMETER_COUNT_MISMATCH
            );

        const auto& parameter = invokable.parameters.front();
        if (static_cast<ETypeQual>(parameter.type.qtype.qual)
                != ETypeQual::LRefToConst
            || parameter.value_type_hash
                != signal.signal->key.payload.hash()
            || parameter.value_type_name
                != signal.signal->key.payload.name())
        {
            return lux::cxx::unexpected(
                EDynamicObserveError::PARAMETER_TYPE_MISMATCH
            );
        }

        auto connected = sender.observeDynamic(
            *signal.signal,
            lux::cxx::move_only_function<void(const void*)>{
                [&receiver, invoker = invokable.invoker](const void* payload)
                {
                    std::array<void*, 1> arguments{
                        const_cast<void*>(payload)
                    };
                    invoker(&receiver, arguments.data(), nullptr);
                }
            },
            receiver,
            delivery
        );
        if (!connected)
            return lux::cxx::unexpected(
                EDynamicObserveError::CONNECTION_REJECTED
            );
        return *connected;
    }
}
