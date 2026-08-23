#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/ObjectFwd.hpp>
#include <lux/engine/object/ObjectWeakRef.hpp>
#include <lux/engine/object/Signal.hpp>

namespace lux::object {
class EventView;
class LuxObject;

enum class EDelivery { DIRECT, QUEUED, AUTO };

enum class EObserveError {
  SENDER_HAS_NO_DISPATCHER,
  RECEIVER_HAS_NO_DISPATCHER,
  DIRECT_CROSS_AFFINITY,
  OBJECT_CLOSED
};

namespace detail {
struct ObjectDiagnosticsAccess;
using ObjectInvokeThunk = void (*)(LuxObject *, const void *, void *) noexcept;

[[nodiscard]] LUX_CORE_PUBLIC bool sendEventErased(LuxObject &target,
                                                   EventView &event) noexcept;

[[nodiscard]] LUX_CORE_PUBLIC lux::cxx::expected<Connection, EObserveError>
observeDynamicErased(LuxObject &sender, const SignalDescriptor &signal,
                     LuxObject &receiver, ObjectInvokeThunk invoke,
                     std::shared_ptr<void> context, EDelivery delivery);
} // namespace detail

class LUX_CORE_PUBLIC LuxObject {
public:
  explicit LuxObject(ObjectDispatcherRef dispatcher = {}) noexcept;
  virtual ~LuxObject();

  LuxObject(const LuxObject &) = delete;
  LuxObject &operator=(const LuxObject &) = delete;
  LuxObject(LuxObject &&) = delete;
  LuxObject &operator=(LuxObject &&) = delete;

  [[nodiscard]] virtual lux::cxx::TypeToken objectType() const noexcept;
  [[nodiscard]] virtual bool
  isObjectType(lux::cxx::TypeToken type) const noexcept;
  [[nodiscard]] ObjectWeakRef weakRef() const;
  [[nodiscard]] std::thread::id affinity() const noexcept { return affinity_; }
  [[nodiscard]] const ObjectDispatcherRef &dispatcherRef() const noexcept {
    return dispatcher_;
  }

protected:
  virtual void event(EventView &) noexcept {}

private:
  template <class Derived, class Base> friend class Object;
  friend class ObjectWeakRef;
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
  friend struct detail::ObjectDiagnosticsAccess;
  [[nodiscard]] std::uint64_t storageGrowthCountForTest() const noexcept;
#endif
  friend bool detail::sendEventErased(LuxObject &, EventView &) noexcept;
  friend lux::cxx::expected<Connection, EObserveError>
  detail::observeDynamicErased(LuxObject &, const detail::SignalDescriptor &,
                               LuxObject &, detail::ObjectInvokeThunk,
                               std::shared_ptr<void>, EDelivery);

  void assertAffinity() const noexcept;
  void notifyIndexed(const detail::SignalDescriptor &signal,
                     const void *payload) noexcept;

  [[nodiscard]] lux::cxx::expected<Connection, EObserveError>
  observeIndexed(const detail::SignalDescriptor &signal, LuxObject &receiver,
                 detail::ObjectInvokeThunk invoke, EDelivery delivery,
                 std::shared_ptr<void> context);
  [[nodiscard]] Connection
  observeStaticIndexed(const detail::SignalDescriptor &signal,
                       detail::ObjectInvokeThunk invoke);
  [[nodiscard]] Connection
  observeCallableIndexed(const detail::SignalDescriptor &signal,
                         detail::ObjectInvokeThunk invoke,
                         std::shared_ptr<void> context);

  [[nodiscard]] lux::cxx::intrusive_ptr<detail::ObjectState>
  ensureState() const;

  mutable std::atomic<detail::ObjectState *> state_{nullptr};
  std::thread::id affinity_;
  ObjectDispatcherRef dispatcher_;
};
} // namespace lux::object
