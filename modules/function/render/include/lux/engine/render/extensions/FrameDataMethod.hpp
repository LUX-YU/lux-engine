#pragma once

#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/cxx/compile_time/fixed_string.hpp>

namespace lux::render
{
    // Compile-time method association. Actual target data is supplied by the installed binding,
    // never by the frame payload; no duplicate Request/Response contract is created.
    template <class T, auto Method, std::uint32_t Dependency = UINT32_MAX,
              cxx::fixed_string Contribution = "", cxx::fixed_string Pass = "">
    struct FrameDataMethod final
    {
    private:
        template <class H, class Target, class Result>
        static H* handlerType(Result (H::*)(Target, T) noexcept);

        template <class H, class Target, class Result>
        static Target targetType(Result (H::*)(Target, T) noexcept);

    public:
        using Data = T;
        using Handler = std::remove_pointer_t<decltype(handlerType(Method))>;
        using Target = decltype(targetType(Method));
        static constexpr auto method = Method;
        static constexpr auto dependency = Dependency;
        static constexpr auto contribution = Contribution;
        static constexpr auto pass = Pass;
    };
}
