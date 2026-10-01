// Synthetic C++20 contract fixture. NOT Lux/lux-cxx/SDK/Process qualification.
#include "InteractionDelivery.example.hpp"
#include <cassert>
namespace d = lux::editor::workbench::detail;
struct Error { int code{}; };
struct Result {
    using value_type = void;
    using error_type = Error;
    bool ok{true};
    Error why{};
    explicit operator bool() const noexcept { return ok; }
    Error& error() & noexcept { return why; }
};
struct OtherError { int code{}; };
struct OtherResult {
    using value_type = void;
    using error_type = OtherError;
    bool ok{true};
    OtherError why{};
    explicit operator bool() const noexcept { return ok; }
    OtherError& error() & noexcept { return why; }
};
struct NonVoidResult : Result { using value_type = int; };
struct RvalueOnly { Result operator()() && { return {}; } };
int main() {
    auto good = []() -> Result { return {}; };
    auto stage = d::EInputDeliveryStage::BEGIN;
#if NEGATIVE_CASE == 1
    auto wrong = []() { return true; };
    (void)d::deliverInput(stage,true,good,wrong,good,good,good);
#elif NEGATIVE_CASE == 2
    auto wrong = []() -> OtherResult { return {}; };
    (void)d::deliverInput(stage,true,good,good,good,wrong,good);
#elif NEGATIVE_CASE == 3
    (void)d::deliverInput(stage,true,good,good,RvalueOnly{},good,good);
#elif NEGATIVE_CASE == 4
    auto wrong = []() -> NonVoidResult { return {}; };
    (void)d::deliverInput(stage,true,wrong,wrong,wrong,wrong,wrong);
#else
    static_assert(d::VoidDeliveryResult<Result>);
    int validations{}, begins{}, previews{}, commits{}, cancels{};
    auto validate = [&]() -> Result { ++validations; return {}; };
    auto cancel = [&]() -> Result { ++cancels; return {}; };
    auto begin = [&]() -> Result { ++begins; return {}; };
    auto preview = [&]() -> Result { ++previews; return {previews != 1, {17}}; };
    auto commit = [&]() -> Result { ++commits; return {}; };
    auto first=d::deliverInput(stage,true,validate,cancel,begin,preview,commit);
    assert(!first && first.error().code==17);
    assert(stage==d::EInputDeliveryStage::PREVIEW && begins==1 && commits==0);
    auto second=d::deliverInput(stage,true,validate,cancel,begin,preview,commit);
    assert(second && stage==d::EInputDeliveryStage::COMPLETE);
    assert(begins==1 && previews==2 && commits==1 && validations==2);
    stage=d::EInputDeliveryStage::CANCEL;
    assert(d::deliverInput(stage,true,validate,cancel,begin,preview,commit));
    assert(cancels==1 && validations==2 && begins==1 && commits==1);
    // Two real production instantiations must still be tested by the implementer.
    return 0;
#endif
}
