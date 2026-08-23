#pragma once

#include <concepts>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <lux/engine/object/LuxObject.hpp>

namespace lux::object {
namespace detail {
template <class Method> struct MemberMethodTraits;

template <class Owner, class Result, class... Args>
struct MemberMethodTraits<Result (Owner::*)(Args...)> {
  using owner_type = Owner;
  using result_type = Result;
  static constexpr std::size_t arity = sizeof...(Args);
  using arguments = std::tuple<Args...>;
};

template <class Owner, class Result, class... Args>
struct MemberMethodTraits<Result (Owner::*)(Args...) const>
    : MemberMethodTraits<Result (Owner::*)(Args...)> {};

template <class Owner, class Result, class... Args>
struct MemberMethodTraits<Result (Owner::*)(Args...) noexcept>
    : MemberMethodTraits<Result (Owner::*)(Args...)> {};

template <class Owner, class Result, class... Args>
struct MemberMethodTraits<Result (Owner::*)(Args...) const noexcept>
    : MemberMethodTraits<Result (Owner::*)(Args...)> {};
} // namespace detail

template <typename Derived, typename Base = LuxObject>
class Object : public Base {
public:
  using Base::Base;

  template <typename Payload = void>
  using signal_type = Signal<Derived, Payload>;

  [[nodiscard]] lux::cxx::TypeToken objectType() const noexcept override {
    return lux::cxx::typeToken<Derived>();
  }

  [[nodiscard]] bool
  isObjectType(lux::cxx::TypeToken type) const noexcept override {
    return type == lux::cxx::typeToken<Derived>() || Base::isObjectType(type);
  }

  template <auto &StaticSignal, class Payload>
  void notify(Payload &&payload) noexcept {
    using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
    using DeclaredPayload = typename SignalType::payload_type;
    static_assert(std::derived_from<Derived, typename SignalType::owner_type>,
                  "Signal must belong to this LuxObject inheritance lineage");
    static_assert(!std::is_void_v<DeclaredPayload>);
    static_assert(std::same_as<std::remove_cvref_t<Payload>, DeclaredPayload>,
                  "Signal payload must exactly match its declaration");
    contractCheckAffinity();
    this->notifyIndexed(StaticSignal.descriptor_, std::addressof(payload));
  }

  template <auto &StaticSignal> void notify() noexcept {
    using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
    static_assert(std::derived_from<Derived, typename SignalType::owner_type>,
                  "Signal must belong to this LuxObject inheritance lineage");
    static_assert(std::is_void_v<typename SignalType::payload_type>);
    contractCheckAffinity();
    this->notifyIndexed(StaticSignal.descriptor_, nullptr);
  }

  template <auto &StaticSignal, auto Method,
            EDelivery Delivery = EDelivery::AUTO, class Receiver>
  [[nodiscard]] lux::cxx::expected<Connection, EObserveError>
  observe(Receiver &receiver)
    requires std::derived_from<Receiver, LuxObject>
  {
    using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
    using Payload = typename SignalType::payload_type;
    using Traits = detail::MemberMethodTraits<decltype(Method)>;
    static_assert(std::derived_from<Derived, typename SignalType::owner_type>,
                  "Signal must belong to this LuxObject inheritance lineage");
    static_assert(std::same_as<typename Traits::result_type, void>);
    static_assert(std::is_base_of_v<typename Traits::owner_type, Receiver>);
    if constexpr (std::is_void_v<Payload>) {
      static_assert(Traits::arity == 0);
      static_assert(
          std::is_nothrow_invocable_r_v<void, decltype(Method), Receiver &>,
          "Signal callbacks must be noexcept");
    } else {
      static_assert(Traits::arity == 1);
      static_assert(
          std::same_as<std::tuple_element_t<0, typename Traits::arguments>,
                       const Payload &>);
      static_assert(std::is_nothrow_invocable_r_v<void, decltype(Method),
                                                  Receiver &, const Payload &>,
                    "Signal callbacks must be noexcept");
      if constexpr (Delivery != EDelivery::DIRECT) {
        static_assert(std::copy_constructible<Payload>,
                      "Queued-capable Signal payload must be copyable");
      }
    }

    contractCheckAffinity();
    return this->observeIndexed(
        StaticSignal.descriptor_, receiver,
        [](LuxObject *object, const void *payload, void *) noexcept {
          auto &typed_receiver = *static_cast<Receiver *>(object);
          if constexpr (std::is_void_v<Payload>)
            (typed_receiver.*Method)();
          else
            (typed_receiver.*Method)(*static_cast<const Payload *>(payload));
        },
        Delivery, {});
  }

  template <auto &StaticSignal, auto Function>
  [[nodiscard]] Connection observe() {
    using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
    using Payload = typename SignalType::payload_type;
    static_assert(std::derived_from<Derived, typename SignalType::owner_type>,
                  "Signal must belong to this LuxObject inheritance lineage");
    if constexpr (std::is_void_v<Payload>)
      static_assert(std::is_nothrow_invocable_r_v<void, decltype(Function)>,
                    "Signal callbacks must be noexcept");
    else
      static_assert(std::is_nothrow_invocable_r_v<void, decltype(Function),
                                                  const Payload &>,
                    "Signal callbacks must be noexcept");

    contractCheckAffinity();
    return this->observeStaticIndexed(
        StaticSignal.descriptor_,
        [](LuxObject *, const void *payload, void *) noexcept {
          if constexpr (std::is_void_v<Payload>)
            Function();
          else
            Function(*static_cast<const Payload *>(payload));
        });
  }

  template <auto &StaticSignal, class Callable>
  [[nodiscard]] ScopedConnection observeScoped(Callable &&callable) {
    using SignalType = std::remove_cvref_t<decltype(StaticSignal)>;
    using Payload = typename SignalType::payload_type;
    using Stored = std::remove_cvref_t<Callable>;
    static_assert(std::derived_from<Derived, typename SignalType::owner_type>,
                  "Signal must belong to this LuxObject inheritance lineage");
    if constexpr (std::is_void_v<Payload>)
      static_assert(std::is_nothrow_invocable_r_v<void, Stored &>,
                    "Signal callbacks must be noexcept");
    else
      static_assert(
          std::is_nothrow_invocable_r_v<void, Stored &, const Payload &>,
          "Signal callbacks must be noexcept");

    contractCheckAffinity();
    auto holder = std::make_shared<Stored>(std::forward<Callable>(callable));
    auto connection = this->observeCallableIndexed(
        StaticSignal.descriptor_,
        [](LuxObject *, const void *payload, void *context) noexcept {
          auto &function = *static_cast<Stored *>(context);
          if constexpr (std::is_void_v<Payload>)
            function();
          else
            function(*static_cast<const Payload *>(payload));
        },
        std::move(holder));
    return ScopedConnection{std::move(connection)};
  }

private:
  void contractCheckAffinity() const noexcept {
#if !defined(NDEBUG) || defined(LUX_OBJECT_CONTRACT_CHECKS)
    this->assertAffinity();
#endif
  }
};
} // namespace lux::object
