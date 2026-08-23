#pragma once

#include <cstddef>
#include <type_traits>

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/ObjectFwd.hpp>
#include <lux/engine/object/detail/SignalDescriptor.hpp>

namespace lux::object {
namespace detail {
struct GeneratedSignalAccess;

LUX_CORE_PUBLIC void invokeQueuedConnection(ConnectionControl *control,
                                            const void *payload) noexcept;

template <class Payload>
[[nodiscard]] MessageEnvelope
makeQueuedSignalMessage(lux::cxx::intrusive_ptr<ConnectionControl> control,
                        const void *payload) noexcept {
  if constexpr (std::is_void_v<Payload>) {
    return makeMessage([control = std::move(control)] {
      invokeQueuedConnection(control.get(), nullptr);
    });
  } else {
    static_assert(std::is_copy_constructible_v<Payload>);
    return makeMessage(
        [control = std::move(control),
         value = Payload(*static_cast<const Payload *>(payload))]() mutable {
          invokeQueuedConnection(control.get(), &value);
        });
  }
}
} // namespace detail

template <class Owner, class Payload = void> class Signal final {
public:
  using owner_type = Owner;
  using payload_type = Payload;

private:
  friend struct detail::GeneratedSignalAccess;
  friend struct detail::ObjectDiagnosticsAccess;
  template <class Derived, class Base> friend class Object;

  constexpr Signal(std::size_t dense_index, std::size_t lineage_size) noexcept
      : descriptor_{dense_index, lineage_size, lux::cxx::typeToken<Owner>(),
                    lux::cxx::typeToken<Payload>(), queueFactory()} {}

  [[nodiscard]] static consteval detail::QueuedMessageFactory queueFactory() {
    if constexpr (std::is_void_v<Payload> ||
                  std::is_copy_constructible_v<Payload>) {
      return &detail::makeQueuedSignalMessage<Payload>;
    } else {
      return nullptr;
    }
  }

  detail::SignalDescriptor descriptor_;
};

namespace detail {
/** Internal code-generation bridge; production source use is gated. */
struct GeneratedSignalAccess final {
  template <class SignalType>
  [[nodiscard]] static consteval SignalType
  make(std::size_t index, std::size_t lineage_size) noexcept {
    return SignalType{index, lineage_size};
  }
};
} // namespace detail

static_assert(std::is_standard_layout_v<Signal<int, int>>);
static_assert(sizeof(Signal<int, int>) == sizeof(detail::SignalDescriptor));
} // namespace lux::object
