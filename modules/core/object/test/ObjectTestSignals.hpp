#pragma once

#include <array>
#include <cstddef>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <utility>

namespace lux::object::test::fixture {
struct MoveOnly final {
  explicit MoveOnly(int input) noexcept : value(input) {}
  MoveOnly(const MoveOnly &) = delete;
  MoveOnly &operator=(const MoveOnly &) = delete;
  MoveOnly(MoveOnly &&) noexcept = default;
  MoveOnly &operator=(MoveOnly &&) noexcept = default;

  int value{0};
};

class LUX_OBJECT() IntSender final : public Object<IntSender> {
public:
  IntSender() = default;
  explicit IntSender(ObjectDispatcherRef dispatcher) noexcept
      : Object(std::move(dispatcher)) {}

  static const signal_type<int> changed;

  void publish(int value) { notify<changed>(value); }
};

class LUX_OBJECT() IntReceiver final : public Object<IntReceiver> {
public:
  LUX_METHOD(connectable = true)
  void receive(const int &value) noexcept { observed += value; }

  std::uint64_t observed{0};
};

class LUX_OBJECT() BaseSender : public Object<BaseSender> {
public:
  static const signal_type<int> baseChanged;

  void publishBase(int value) { notify<baseChanged>(value); }
};

class LUX_OBJECT() DerivedSender final
    : public Object<DerivedSender, BaseSender> {};

class LUX_OBJECT() MultiSender final : public Object<MultiSender> {
public:
  static const signal_type<int> changed;
  static const signal_type<MoveOnly> moveOnly;

  void publish(int value) { notify<changed>(value); }
  void publish(MoveOnly &value) { notify<moveOnly>(value); }
};

class LUX_OBJECT() VoidSender final : public Object<VoidSender> {
public:
  static const signal_type<> closing;

  void publish() { notify<closing>(); }
};

template <std::size_t Size> struct QueuedPayload final {
  std::array<std::byte, Size> bytes{};
};

using QueuedPayload4 = QueuedPayload<4>;
using QueuedPayload64 = QueuedPayload<64>;
using QueuedPayload256 = QueuedPayload<256>;
using QueuedPayload1024 = QueuedPayload<1024>;

class LUX_OBJECT() QueuedPayloadSender final
    : public Object<QueuedPayloadSender> {
public:
  explicit QueuedPayloadSender(ObjectDispatcherRef dispatcher) noexcept
      : Object(std::move(dispatcher)) {}

  static const signal_type<QueuedPayload4> payload4;
  static const signal_type<QueuedPayload64> payload64;
  static const signal_type<QueuedPayload256> payload256;
  static const signal_type<QueuedPayload1024> payload1024;

  void publish(const QueuedPayload4 &value) noexcept {
    notify<payload4>(value);
  }
  void publish(const QueuedPayload64 &value) noexcept {
    notify<payload64>(value);
  }
  void publish(const QueuedPayload256 &value) noexcept {
    notify<payload256>(value);
  }
  void publish(const QueuedPayload1024 &value) noexcept {
    notify<payload1024>(value);
  }
};

class LUX_OBJECT() QueuedPayloadReceiver final
    : public Object<QueuedPayloadReceiver> {
public:
  explicit QueuedPayloadReceiver(ObjectDispatcherRef dispatcher) noexcept
      : Object(std::move(dispatcher)) {}

  void receive(const QueuedPayload4 &) noexcept { ++observed; }
  void receive(const QueuedPayload64 &) noexcept { ++observed; }
  void receive(const QueuedPayload256 &) noexcept { ++observed; }
  void receive(const QueuedPayload1024 &) noexcept { ++observed; }

  std::uint64_t observed{0};
};
} // namespace lux::object::test::fixture
