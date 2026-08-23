#pragma once

#include <optional>
#include <string_view>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/Signal.hpp>

namespace lux::object::reflection {
struct SignalViewAccess;

enum class EDynamicObserveError {
  INVALID_SIGNAL,
  SIGNAL_OWNER_MISMATCH,
  METHOD_NOT_CONNECTABLE,
  METHOD_MUST_BE_INSTANCE,
  RECEIVER_TYPE_MISMATCH,
  RETURN_TYPE_MISMATCH,
  PARAMETER_COUNT_MISMATCH,
  PARAMETER_TYPE_MISMATCH,
  PAYLOAD_NOT_QUEUEABLE,
  CONNECTION_REJECTED
};

class SignalView;

[[nodiscard]] LUX_CORE_PUBLIC SignalView findDeclaredSignal(
    const lux::meta::RefClass &object_class, std::string_view name) noexcept;

[[nodiscard]] LUX_CORE_PUBLIC SignalView findSignal(
    const lux::meta::ReflectionRegistry &registry,
    const lux::meta::RefClass &object_class, std::string_view name) noexcept;

[[nodiscard]] LUX_CORE_PUBLIC
    lux::cxx::expected<Connection, EDynamicObserveError>
    observe(lux::object::LuxObject &sender, SignalView signal,
            lux::object::LuxObject &receiver,
            const lux::meta::RefMethod &method,
            lux::object::EDelivery delivery = lux::object::EDelivery::AUTO);

class SignalView final {
public:
  SignalView() noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ != nullptr && field_ != nullptr;
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return field_ ? field_->name : std::string_view{};
  }
  [[nodiscard]] lux::cxx::TypeToken ownerType() const noexcept {
    return descriptor_ ? descriptor_->owner_ : lux::cxx::TypeToken{};
  }
  [[nodiscard]] lux::cxx::TypeToken payloadType() const noexcept {
    return descriptor_ ? descriptor_->payload_ : lux::cxx::TypeToken{};
  }
  [[nodiscard]] bool hasPayload() const noexcept {
    return descriptor_ && descriptor_->payload_ != lux::cxx::typeToken<void>();
  }

private:
  friend struct SignalViewAccess;

  SignalView(const detail::SignalDescriptor *descriptor,
             const lux::meta::RefStaticField *field) noexcept
      : descriptor_(descriptor), field_(field) {}

  [[nodiscard]] bool queueable() const noexcept {
    return descriptor_ && descriptor_->queued_message_factory_ != nullptr;
  }

  const detail::SignalDescriptor *descriptor_{nullptr};
  const lux::meta::RefStaticField *field_{nullptr};
};

} // namespace lux::object::reflection
