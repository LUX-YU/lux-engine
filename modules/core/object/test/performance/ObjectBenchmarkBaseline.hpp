#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(LUX_OBJECT_BENCHMARK_BASELINE_LIBRARY)
#define LUX_OBJECT_BENCHMARK_BASELINE_API __declspec(dllexport)
#else
#define LUX_OBJECT_BENCHMARK_BASELINE_API __declspec(dllimport)
#endif
#else
#define LUX_OBJECT_BENCHMARK_BASELINE_API __attribute__((visibility("default")))
#endif

namespace lux::object::test::benchmark
{
    class LUX_OBJECT_BENCHMARK_BASELINE_API BaselineReceiver
    {
    public:
        void member(int value) noexcept;
        virtual void virtualMember(int value) noexcept;
        virtual ~BaselineReceiver();

        [[nodiscard]] std::uint64_t observed() const noexcept;

    private:
        std::uint64_t observed_{0};
    };

    using BaselineFunction = void (*)(BaselineReceiver&, int) noexcept;

    [[nodiscard]] LUX_OBJECT_BENCHMARK_BASELINE_API BaselineReceiver* createBaselineReceiver();
    LUX_OBJECT_BENCHMARK_BASELINE_API void destroyBaselineReceiver(BaselineReceiver*) noexcept;
    [[nodiscard]] LUX_OBJECT_BENCHMARK_BASELINE_API BaselineFunction baselineFunction() noexcept;
} // namespace lux::object::test::benchmark
