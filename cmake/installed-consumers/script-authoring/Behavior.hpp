#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>

namespace authoring_consumer
{
    inline std::int32_t cpp_total{};
    class LUX_TYPE_INFO(compile_time) Behavior final
    {
    public:
        LUX_METHOD(script_export="common.update", script_suggest_hook="Host.first")
        void update(std::int32_t input) noexcept
        {
            sum_ += input;
            cpp_total += sum_;
        }
    private:
        std::int32_t sum_{};
    };
}
