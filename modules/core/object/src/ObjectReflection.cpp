#include <lux/engine/object/ObjectReflection.hpp>

#include <array>
#include <exception>
#include <memory>
#include <utility>

namespace lux::object::reflection {
struct SignalViewAccess final {
  [[nodiscard]] static SignalView
  make(const detail::SignalDescriptor *descriptor,
       const lux::meta::RefStaticField *field) noexcept {
    return SignalView{descriptor, field};
  }

  [[nodiscard]] static const detail::SignalDescriptor &
  descriptor(SignalView view) noexcept {
    return *view.descriptor_;
  }

  [[nodiscard]] static bool queueable(SignalView view) noexcept {
    return view.queueable();
  }
};

SignalView findDeclaredSignal(const lux::meta::RefClass &object_class,
                              std::string_view name) noexcept {
  for (const auto &field : object_class.static_fields) {
    const bool has_matching_name = field.name == name;
    const bool has_signal_annotation = field.annotations().has("signal");
    const bool has_signal_type =
        field.template_primary == "lux::object::Signal";
    const bool has_address = field.address;
    const bool has_expected_size =
        field.type.size == sizeof(detail::SignalDescriptor);
    const bool is_declared_signal = has_matching_name &&
                                    has_signal_annotation && has_signal_type &&
                                    has_address && has_expected_size;
    if (!is_declared_signal) {
      continue;
    }
    return SignalViewAccess::make(
        reinterpret_cast<const detail::SignalDescriptor *>(field.address),
        std::addressof(field));
  }
  return {};
}

SignalView findSignal(const lux::meta::ReflectionRegistry &registry,
                      const lux::meta::RefClass &object_class,
                      std::string_view name) noexcept {
  if (auto declared = findDeclaredSignal(object_class, name))
    return declared;
  for (const auto parent_hash : object_class.parent_chain) {
    const auto *parent = registry.findClassByHash(parent_hash);
    if (parent) {
      if (auto inherited = findDeclaredSignal(*parent, name))
        return inherited;
    }
  }
  return {};
}

lux::cxx::expected<Connection, EDynamicObserveError>
observe(lux::object::LuxObject &sender, SignalView signal,
        lux::object::LuxObject &receiver, const lux::meta::RefMethod &method,
        lux::object::EDelivery delivery) {
  using lux::meta::EBaseType;
  using lux::meta::ETypeQual;

  if (!signal) {
    return lux::cxx::unexpected(EDynamicObserveError::INVALID_SIGNAL);
  }
  if (!sender.isObjectType(signal.ownerType())) {
    return lux::cxx::unexpected(EDynamicObserveError::SIGNAL_OWNER_MISMATCH);
  }
  if (!method.annotations().has("connectable")) {
    return lux::cxx::unexpected(EDynamicObserveError::METHOD_NOT_CONNECTABLE);
  }

  if (method.is_static || !method.invokable.invoker) {
    return lux::cxx::unexpected(EDynamicObserveError::METHOD_MUST_BE_INSTANCE);
  }

  const bool has_owner_class = method.owner_class != nullptr;
  const bool has_matching_receiver_type =
      has_owner_class &&
      receiver.isObjectType(lux::cxx::TypeToken{method.owner_class->type.hash,
                                                method.owner_class->full_name});
  if (!has_owner_class || !has_matching_receiver_type) {
    return lux::cxx::unexpected(EDynamicObserveError::RECEIVER_TYPE_MISMATCH);
  }

  const auto &invokable = method.invokable;
  if (static_cast<EBaseType>(invokable.return_type.qtype.base) !=
          EBaseType::Void ||
      static_cast<ETypeQual>(invokable.return_type.qtype.qual) !=
          ETypeQual::Value) {
    return lux::cxx::unexpected(EDynamicObserveError::RETURN_TYPE_MISMATCH);
  }
  const auto expected_parameter_count =
      signal.hasPayload() ? std::size_t{1} : std::size_t{0};
  if (invokable.parameters.size() != expected_parameter_count) {
    return lux::cxx::unexpected(EDynamicObserveError::PARAMETER_COUNT_MISMATCH);
  }

  if (signal.hasPayload()) {
    const auto &parameter = invokable.parameters.front();
    if (static_cast<ETypeQual>(parameter.type.qtype.qual) !=
            ETypeQual::LRefToConst ||
        parameter.value_type_hash != signal.payloadType().hash() ||
        parameter.value_type_name != signal.payloadType().name()) {
      return lux::cxx::unexpected(
          EDynamicObserveError::PARAMETER_TYPE_MISMATCH);
    }
  }

  struct DynamicInvoke final {
    decltype(invokable.invoker) invoker{nullptr};
    bool has_payload{false};
  };
  auto context = std::make_shared<DynamicInvoke>(
      DynamicInvoke{invokable.invoker, signal.hasPayload()});
  const bool queued =
      delivery == EDelivery::QUEUED ||
      (delivery == EDelivery::AUTO && sender.affinity() != receiver.affinity());
  if (queued && !SignalViewAccess::queueable(signal)) {
    return lux::cxx::unexpected(EDynamicObserveError::PAYLOAD_NOT_QUEUEABLE);
  }
  auto connected = detail::observeDynamicErased(
      sender, SignalViewAccess::descriptor(signal), receiver,
      [](LuxObject *object, const void *payload, void *raw_context) noexcept {
        auto &invoke = *static_cast<DynamicInvoke *>(raw_context);
        try {
          if (invoke.has_payload) {
            std::array<void *, 1> arguments{const_cast<void *>(payload)};
            invoke.invoker(object, arguments.data(), nullptr);
          } else {
            invoke.invoker(object, nullptr, nullptr);
          }
        } catch (...) {
          std::terminate();
        }
      },
      std::move(context), delivery);
  if (!connected) {
    return lux::cxx::unexpected(EDynamicObserveError::CONNECTION_REJECTED);
  }
  return *connected;
}
} // namespace lux::object::reflection
