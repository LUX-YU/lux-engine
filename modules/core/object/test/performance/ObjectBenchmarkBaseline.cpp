#include "ObjectBenchmarkBaseline.hpp"

namespace lux::object::test::benchmark {
void BaselineReceiver::member(int value) noexcept {
  observed_ += static_cast<std::uint64_t>(value);
}

void BaselineReceiver::virtualMember(int value) noexcept {
  observed_ += static_cast<std::uint64_t>(value);
}

BaselineReceiver::~BaselineReceiver() = default;

std::uint64_t BaselineReceiver::observed() const noexcept { return observed_; }

BaselineReceiver *createBaselineReceiver() { return new BaselineReceiver; }

void destroyBaselineReceiver(BaselineReceiver *receiver) noexcept {
  delete receiver;
}

namespace {
void invokeBaseline(BaselineReceiver &receiver, int value) noexcept {
  receiver.member(value);
}
} // namespace

BaselineFunction baselineFunction() noexcept { return &invokeBaseline; }
} // namespace lux::object::test::benchmark
