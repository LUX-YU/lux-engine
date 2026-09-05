#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>

namespace invalid_generated
{
    struct NonTrivial final
    {
        NonTrivial(const NonTrivial&) noexcept {}
        ~NonTrivial() noexcept {}
        std::int32_t value{};
    };
}
namespace lux::semantic
{
    template <> struct TypeTraits<invalid_generated::NonTrivial>
    {
        inline static constexpr std::string_view CanonicalName{"test.NonTrivial"};
        inline static constexpr std::uint8_t AbiKind{LUX_SCRIPT_VK_STRUCT_REF};
        inline static constexpr std::uint32_t Size{sizeof(invalid_generated::NonTrivial)};
        inline static constexpr std::uint32_t Alignment{alignof(invalid_generated::NonTrivial)};
    };
}
namespace invalid_generated
{
    using lux::simulation::script::ScriptCoroutine;
    using lux::simulation::script::ScriptCoroutineContext;
    class LUX_TYPE_INFO(compile_time) Behavior final
    {
    public:
#if CPP_INVALID_CASE == 1
        LUX_METHOD(script_export="negative.entry", script_coroutine=true)
        ScriptCoroutine run(ScriptCoroutineContext&, const NonTrivial&) noexcept;
#elif CPP_INVALID_CASE == 2
        LUX_METHOD(script_export="negative.entry")
        void run(std::int32_t*) noexcept;
#elif CPP_INVALID_CASE == 3
        LUX_METHOD(script_export="negative.entry")
        void run(std::int32_t&) noexcept;
#elif CPP_INVALID_CASE == 4
        LUX_METHOD(script_export="negative.entry")
        void run();
#elif CPP_INVALID_CASE == 5
        LUX_METHOD(script_export="negative.entry", script_coroutine=true, script_lifecycle=begin_play)
        ScriptCoroutine run(ScriptCoroutineContext&) noexcept;
#elif CPP_INVALID_CASE == 6
        LUX_METHOD(script_export="negative.missing")
        void run() noexcept;
#elif CPP_INVALID_CASE == 7
    private:
        LUX_METHOD(script_export="negative.entry")
        void run() noexcept;
#elif CPP_INVALID_CASE == 8
        LUX_METHOD(script_export="negative.entry", script_lifecycle=end_play)
        void run(std::uint32_t) noexcept;
#elif CPP_INVALID_CASE == 9
        LUX_METHOD(script_export="negative.entry")
        const std::int32_t& run() noexcept;
#elif CPP_INVALID_CASE == 10
        LUX_METHOD(script_export="negative.entry", script_coroutine=true)
        ScriptCoroutine run(std::int32_t) noexcept;
#elif CPP_INVALID_CASE == 11
        LUX_METHOD(script_export="negative.entry")
        void run(std::int32_t&&) noexcept;
#elif CPP_INVALID_CASE == 12
        LUX_METHOD(script_export="negative.entry")
        void run(...) noexcept;
#elif CPP_INVALID_CASE == 13
        LUX_METHOD(script_export="negative.entry")
        void run() noexcept;
        LUX_METHOD(script_export="negative.entry")
        void again() noexcept;
#endif
    };
}
